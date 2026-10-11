#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "NAM/dsp.h"

// Chaîne : gate -> gain d'entrée -> NAM -> IR -> EQ 3 bandes -> delay -> reverb -> volume
class ToneForgeProcessor : public juce::AudioProcessor
{
public:
    ToneForgeProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ToneForge"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // Chargement depuis le thread de l'interface ; renvoie un message d'erreur vide si OK.
    juce::String loadModel(const juce::File&);
    juce::String loadIR(const juce::File&);
    juce::File modelFile, irFile;
    juce::String modelWarning;   // ex. fréquence d'échantillonnage différente

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    void updateEq();

    juce::SpinLock modelLock;
    std::unique_ptr<nam::DSP> model;
    std::vector<float> namIn, namOut;

    juce::dsp::NoiseGate<float> gate;
    juce::dsp::Convolution cab { juce::dsp::Convolution::NonUniform { 256 } };
    bool irLoaded = false;
    using Filter = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    juce::dsp::ProcessorChain<Filter, Filter, Filter> eq;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delay { 192000 };
    juce::dsp::Reverb reverb;
    float lastEq[3] { 99.f, 99.f, 99.f };
    double sr = 48000.0;
    int maxBlock = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneForgeProcessor)
};
