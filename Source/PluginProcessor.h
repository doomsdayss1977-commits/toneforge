#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "NAM/dsp.h"
#include "Effects.h"

inline juce::String u8(const char* s) { return juce::String(juce::CharPointer_UTF8(s)); }
// Définis dans PluginEditor.cpp (bibliothèque et marques partagées avec le serveur OSC)
juce::File libraryRoot();
juce::String captureBrand(const juce::String& s, bool pedal);
class OscServer;

// Routage (modèle : chaîne d'AmpliTube) : [avant, mono] -> séparation -> chaînes A et B en parallèle -> mélangeur
// (balance, volume, panoramique) -> [après, stéréo] -> volume. Toute unité va n'importe où, une fois (cf. getRoute).
class ToneForgeProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    ToneForgeProcessor();
    ~ToneForgeProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ToneForge"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }   // queue du delay et de la reverb

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // Chargement depuis le thread de l'interface ; renvoie un message d'erreur vide si OK.
    // Réserves (paramètres statiques) : NAM 0 ampli A, 1 ampli B, 2..5 pédales 1..4, 6..7 libres ; IR 0 baffle A, 1 baffle B, 2..3
    static constexpr int kPedals = 4, kNams = 8, kIrs = 4, kEqs = 2, kGates = 2, kFxCopies = 2;
    static juce::String pedalId(int i) { return i == 0 ? juce::String("pedal") : "pedal" + juce::String(i + 1); }   // préfixe des paramètres
    static juce::String namParamId(int i, int what);   // what : 0 marche, 1 entrée, 2 sortie, 3 tone, 4 blend
    juce::String loadNam(int i, const juce::File&);
    void unloadNam(int i);
    juce::String loadIr(int i, const juce::File&);
    void unloadIr(int i);
    juce::String loadModel(const juce::File& f, bool isPedal = false, bool ampB = false, int slot = 0)
    {
        return loadNam(isPedal ? 2 + slot : ampB ? 1 : 0, f);
    }
    void unloadPedal(int slot) { unloadNam(2 + slot); }
    juce::String loadIR(const juce::File& f, bool cabB = false) { return loadIr(cabB ? 1 : 0, f); }
    void unloadB(bool model, bool ir)   // retire la capture et/ou l'IR de l'ampli B
    {
        if (model)
            unloadNam(1);
        if (ir)
            unloadIr(1);
    }
    juce::File namFiles[kNams], irFiles[kIrs];   // écrits sous fileLock (setStateInformation peut venir d'un autre fil)
    juce::File &modelFile = namFiles[0], &modelBFile = namFiles[1], &irFile = irFiles[0], &irBFile = irFiles[1];
    juce::File* const pedalFiles = namFiles + 2;
    // Genre de fichier : 0 ampli, 1 pédale 1, 2 IR, 3 ampli B, 4 IR B, 5..7 pédales 2..4, 8..9 NAM 7..8, 10..11 IR 3..4
    static int pedalKind(int slot) { return slot == 0 ? 1 : 4 + slot; }
    static int pedalSlotOf(int kind) { return kind == 1 ? 0 : kind >= 5 && kind <= 7 ? kind - 4 : -1; }
    static int namOfKind(int kind)
    {
        const int p = pedalSlotOf(kind);
        return p >= 0 ? 2 + p : kind == 0 ? 0 : kind == 3 ? 1 : kind == 8 || kind == 9 ? kind - 2 : -1;
    }
    static int irOfKind(int kind) { return kind == 2 ? 0 : kind == 4 ? 1 : kind == 10 || kind == 11 ? kind - 8 : -1; }
    juce::File loadedFile(int kind) const   // copie sous verrou
    {
        const juce::ScopedLock sl(fileLock);
        const int i = namOfKind(kind);
        return i >= 0 ? namFiles[i] : irFiles[juce::jmax(0, irOfKind(kind))];
    }
    juce::String modelWarning(int kind = 0) const   // captures NAM ; ex. capture rééchantillonnée
    {
        const auto& s = nams[juce::jmax(0, namOfKind(kind))];
        if (kind == 3 && !s.loaded)
            return u8("Aucune capture B : clique un ampli dans la liste (le bloc B ne fait rien sans elle)");
        const int c = s.captureRate, p = s.projectRate;
        return c > 0 ? u8("Capture à ") + juce::String(c / 1000.0, 1) + u8(" kHz, projet à ") + juce::String(p / 1000.0, 1)
                           + u8(" kHz : rééchantillonnée")
                     : juce::String();
    }

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> inPeak { 0.f }, outPeak { 0.f };   // crêtes lues (et remises à 0) par les vumètres
    std::atomic<float> oscInPeak { 0.f }, oscOutPeak { 0.f };   // idem pour le serveur OSC
    std::atomic<float> dynPos { 0.f };   // part de B réellement jouée (indicateur de l'attaque)
    juce::CriticalSection fileLock;
    std::atomic<int> loadSerial { 0 };   // incrémenté à chaque capture/IR chargée (l'interface se met à jour)

    // Accordeur : pendant l'accordage la sortie est coupée et l'entrée brute (canal 0) va dans un anneau.
    std::atomic<bool> tunerOn { false };
    void readTuner(std::vector<float>& out, int n, int dec) const;   // n derniers points, moyennés par dec

    // Looper (modèle : LOOP1 d'AmpliTube) : enregistre la sortie et la rejoue ; métronome par-dessus, jamais enregistré.
    enum { loopEmpty, loopRec, loopPlay, loopDub, loopStop, loopClear };
    std::atomic<int> loopState { loopEmpty }, loopCmd { -1 }, loopLen { 0 }, loopPos { 0 };   // loopCmd : appliquée par le thread audio
    std::atomic<bool> metroOn { false };
    std::atomic<float> metroBpm { 120.f };
    int loopMax() const { return loopBuf.getNumSamples(); }

    // Macro (modèle : <MidiAssignments> Min/Max/Curve d'AmpliTube) : le paramètre « macro » pilote plusieurs
    // paramètres, chacun entre son min et son max (valeurs normalisées) selon sa courbe (0 linéaire, 1 lente, 2 rapide).
    // Cibles = enfants « T » (id, min, max, curve) de l'arbre « Macro » de l'état : sauvées avec le preset.
    // Toujours sous stateLock : setStateInformation peut remplacer l'état depuis un autre fil.
    juce::ValueTree macroTree() { return apvts.state.getOrCreateChildWithName("Macro", nullptr); }
    juce::CriticalSection stateLock;

    // Routage : avant « split » chaîne A « | » chaîne B « mix » après. Unités : gate1-2, eq1-2, ir1-4, nam1-8, effets
    // (clé, et clé + « 2 » pour le 2e exemplaire). Absente = retirée. Sauvé dans l'état (propriété « chain »).
    // La chaîne B ne joue que si « ampb_on » ; la plus courte des deux attend l'autre (latences NAM).
    static juce::StringArray defaultRoute();
    juce::StringArray getRoute() const { const juce::ScopedLock sl(chainNamesLock); return chainNames; }
    void setRoute(const juce::StringArray&);   // thread de messages ; corrige (noms connus, sans doublon, repères complets)
    // Vue de l'ancienne interface (pédales et effets autour de « amp » = tout le reste) : à retirer avec elle
    juce::StringArray getChain() const;
    void setChain(const juce::StringArray&);

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    void updateEq(int i);

    // Rééchantillonnage autour de NAM quand la capture n'est pas au taux du projet (protégé par le verrou de l'emplacement).
    struct Resampler
    {
        bool on = false;
        double ratio = 1.0;   // taux de la capture / taux du projet
        juce::Interpolators::Lagrange up, down;
        std::vector<float> in, mid, out;   // file d'entrée (projet), bloc NAM, file de sortie (capture)
        std::vector<juce::dsp::IIR::Filter<float>> aa;   // anti-repliement avant l'étage qui baisse le taux
        int inN = 0, outN = 0;
    };
    // Emplacement de capture NAM (pédale ou ampli). Le thread audio n'y prend le verrou qu'en try-lock.
    struct NamSlot
    {
        juce::SpinLock lock;
        std::unique_ptr<nam::DSP> model;
        std::vector<float> in, out;
        Resampler rs;
        std::atomic<int> captureRate { 0 }, projectRate { 0 };   // capture rééchantillonnée (0 = non), lus par l'interface
        std::atomic<bool> loaded { false };   // une capture est chargée (lu sans verrou par le thread audio)
        std::atomic<float> latency { 0.f };   // retard du rééchantillonnage, en échantillons du projet (mesuré)
    } nams[kNams];
    // Pédale : Tone (plateau d'aigus après la capture), Blend (mélange avec le son direct, retardé comme la capture)
    struct PedalExtra
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0, tone = -1.f;
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> dry { 512 };
    } namX[kNams];
    std::atomic<float>* namPar[kNams][5] {};   // on, in, out, tone, mix (cache des paramètres)
    std::vector<float> dryBuf;   // son direct d'une pédale (maxBlock)
    void runNamUnit(int i, float* x, int n);
    float namLatency(int i) const;   // 0 si éteinte ou vide
    float rowLatency(int from, int to) const;   // NAM de chainCur.u[from..to)
    // Routage côté audio : codes d'unité (cf. unitCode dans le .cpp)
    struct Chain { int n = 0; int u[40] {}; } chainPending, chainCur;
    juce::SpinLock chainLock;   // le thread audio ne fait qu'un try-lock
    std::atomic<bool> chainDirty { false };
    mutable juce::CriticalSection chainNamesLock;
    juce::StringArray chainNames;
    void publishChain(const juce::StringArray&);
    void runUnit(int u, float* l, float* r, int n);   // r nul : unité en mono (canal droit fictif)
    void configureModel(nam::DSP&, NamSlot&);
    static void processResampled(Resampler&, nam::DSP*, float* x, int n);   // DSP nul = identité (mesure)
    void runNam(NamSlot&, float* x, int n, float gainIn, float gainOut);

    juce::dsp::NoiseGate<float> gates[kGates];
    std::atomic<float>* gatePar[kGates][2] {};   // on, seuil
    using NU = juce::dsp::Convolution::NonUniform;
    juce::dsp::Convolution cabs[kIrs] { juce::dsp::Convolution { NU { 256 } }, juce::dsp::Convolution { NU { 256 } },
                                        juce::dsp::Convolution { NU { 256 } }, juce::dsp::Convolution { NU { 256 } } };
    std::atomic<bool> irLoaded[kIrs] {};
    std::atomic<float>* irOn[kIrs] {};
    std::vector<float> bufB;   // voie B (maxBlock)
    // Alignement A/B : la voie la moins retardée attend l'autre (sinon filtrage en peigne au mélange)
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> alignA { 512 }, alignB { 512 };
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    juce::dsp::ProcessorChain<Filter, Filter, Filter> eqs[kEqs];
    std::atomic<float>* eqPar[kEqs][3] {};
    std::vector<std::unique_ptr<AudioEffect>> fx;   // un par entrée de fxDefs()
    std::vector<std::atomic<float>*> fxOn;               // paramètres en cache (pas d'allocation en audio)
    std::vector<std::vector<std::atomic<float>*>> fxPar;
    // Instance neuve de rechange par effet : échangée à la réactivation (tampons vides),
    // recréée hors du thread audio par timerCallback. spareReady dit qui y a droit.
    std::vector<std::unique_ptr<AudioEffect>> spare;
    std::unique_ptr<std::atomic<bool>[]> spareReady;
    std::vector<char> lastOn;
    std::vector<float> scratch;                      // canal droit fictif pour les effets en mono
    void runFx(size_t k, float* l, float* r, int n);
    void timerCallback() override;
    float lastEq[kEqs][3] {};
    double sr = 48000.0;
    static constexpr int kRing = 32768;
    float ring[kRing] {};
    std::atomic<int> ringPos { 0 };
    int maxBlock = 512;
    std::atomic<float> lastMacro { -1.f };   // écrit aussi par setStateInformation
    bool bActive = false;   // voie B active au bloc précédent (thread audio)
    float dynEnv = 0.f;   // enveloppe de l'attaque, entrée brute (thread audio)
    std::vector<float> dynMix;   // part de B voulue par l'attaque, par échantillon
    juce::AudioBuffer<float> loopBuf;   // 2 canaux, 60 s
    int clickPos = 1 << 30, clickBeat = 0;
    double sinceBeat = 1e9;
    void runLooper(juce::AudioBuffer<float>&, int n);
    std::unique_ptr<OscServer> osc;   // dernier membre : détruit en premier

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneForgeProcessor)
};
