#pragma once
#include "PluginProcessor.h"

class ToneForgeEditor : public juce::AudioProcessorEditor
{
public:
    explicit ToneForgeEditor(ToneForgeProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void pick(bool isModel);
    void refreshLabels();

    ToneForgeProcessor& proc;
    juce::TextButton modelBtn { "Capture NAM..." }, irBtn { "IR baffle..." };
    juce::Label modelLbl, irLbl, warnLbl;
    std::unique_ptr<juce::FileChooser> chooser;

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    std::vector<std::unique_ptr<Knob>> knobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneForgeEditor)
};
