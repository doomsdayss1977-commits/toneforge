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

// Chaîne : gate -> compresseur -> pédale NAM -> gain d'entrée -> ampli NAM -> IR -> EQ 3 bandes -> chorus -> delay -> reverb -> volume
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
    juce::String loadModel(const juce::File&, bool isPedal = false, bool ampB = false);
    juce::String loadIR(const juce::File&, bool cabB = false);
    void unloadB(bool model, bool ir);   // retire la capture et/ou l'IR de l'ampli B
    juce::File modelFile, pedalFile, irFile, modelBFile, irBFile;   // écrits sous fileLock (setStateInformation peut venir d'un autre fil)
    juce::File loadedFile(int kind) const   // 0 ampli, 1 pédale, 2 IR, 3 ampli B, 4 IR B ; copie sous verrou
    {
        const juce::ScopedLock sl(fileLock);
        return kind == 0 ? modelFile : kind == 1 ? pedalFile : kind == 2 ? irFile : kind == 3 ? modelBFile : irBFile;
    }
    juce::String modelWarning(int kind = 0) const   // 0 ampli, 1 pédale, 3 ampli B ; ex. capture rééchantillonnée
    {
        const auto& s = kind == 1 ? pedal : kind == 3 ? ampB : amp;
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

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    void updateEq();

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
    } pedal, amp, ampB;   // ampB : ampli B en parallèle de A
    void configureModel(nam::DSP&, NamSlot&);
    static void processResampled(Resampler&, nam::DSP*, float* x, int n);   // DSP nul = identité (mesure)
    void runNam(NamSlot&, float* x, int n, float gainIn, float gainOut);

    juce::dsp::NoiseGate<float> gate;
    juce::dsp::Convolution cab { juce::dsp::Convolution::NonUniform { 256 } };
    juce::dsp::Convolution cabB { juce::dsp::Convolution::NonUniform { 256 } };
    std::atomic<bool> irLoaded { false }, irBLoaded { false };
    std::vector<float> bufB;   // voie B (maxBlock)
    // Alignement A/B : la voie la moins retardée attend l'autre (sinon filtrage en peigne au mélange)
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> alignA { 512 }, alignB { 512 };
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    juce::dsp::ProcessorChain<Filter, Filter, Filter> eq;
    std::vector<std::unique_ptr<AudioEffect>> fx;   // un par entrée de fxDefs()
    std::vector<std::atomic<float>*> fxOn;               // paramètres en cache (pas d'allocation en audio)
    std::vector<std::vector<std::atomic<float>*>> fxPar;
    // Instance neuve de rechange par effet : échangée à la réactivation (tampons vides),
    // recréée hors du thread audio par timerCallback. spareReady dit qui y a droit.
    std::vector<std::unique_ptr<AudioEffect>> spare;
    std::unique_ptr<std::atomic<bool>[]> spareReady;
    std::vector<char> lastOn;
    std::vector<float> scratch;                      // canal droit fictif pour les effets en mono
    void runFx(bool preAmp, float* l, float* r, int n);
    void timerCallback() override;
    float lastEq[3] { 99.f, 99.f, 99.f };
    double sr = 48000.0;
    static constexpr int kRing = 32768;
    float ring[kRing] {};
    std::atomic<int> ringPos { 0 };
    int maxBlock = 512;
    std::atomic<float> lastMacro { -1.f };   // écrit aussi par setStateInformation
    bool bActive = false;   // voie B active au bloc précédent (thread audio)
    juce::AudioBuffer<float> loopBuf;   // 2 canaux, 60 s
    int clickPos = 1 << 30, clickBeat = 0;
    double sinceBeat = 1e9;
    void runLooper(juce::AudioBuffer<float>&, int n);
    std::unique_ptr<OscServer> osc;   // dernier membre : détruit en premier

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneForgeProcessor)
};
