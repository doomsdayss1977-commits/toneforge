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
    juce::String loadModel(const juce::File&, bool isPedal = false);
    juce::String loadIR(const juce::File&);
    juce::File modelFile, pedalFile, irFile;   // écrits sous fileLock (setStateInformation peut venir d'un autre fil)
    juce::File loadedFile(int kind) const   // 0 ampli, 1 pédale, 2 IR ; copie sous verrou
    {
        const juce::ScopedLock sl(fileLock);
        return kind == 0 ? modelFile : kind == 1 ? pedalFile : irFile;
    }
    juce::String modelWarning(bool isPedal = false) const   // ex. capture rééchantillonnée ; suit les changements de taux du projet
    {
        const auto& s = isPedal ? pedal : amp;
        const int c = s.captureRate, p = s.projectRate;
        return c > 0 ? "Capture à " + juce::String(c) + " Hz, rééchantillonnée depuis " + juce::String(p) + " Hz"
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
    } pedal, amp;
    void configureModel(nam::DSP&, NamSlot&);
    void processResampled(NamSlot&, float* x, int n);
    void runNam(NamSlot&, float* x, int n, float gainIn, float gainOut);

    juce::dsp::NoiseGate<float> gate;
    juce::dsp::Convolution cab { juce::dsp::Convolution::NonUniform { 256 } };
    std::atomic<bool> irLoaded { false };
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
    std::unique_ptr<OscServer> osc;   // dernier membre : détruit en premier

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneForgeProcessor)
};
