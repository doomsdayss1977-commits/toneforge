#include "PluginEditor.h"

static const char* kIds[] = { "gate", "input", "bass", "mid", "treble", "dtime", "dfb", "dmix", "rsize", "rmix", "output" };

ToneForgeEditor::ToneForgeEditor(ToneForgeProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    for (auto* b : { &modelBtn, &irBtn })
        addAndMakeVisible(b);
    modelBtn.onClick = [this] { pick(true); };
    irBtn.onClick = [this] { pick(false); };
    for (auto* l : { &modelLbl, &irLbl, &warnLbl })
        addAndMakeVisible(l);
    warnLbl.setColour(juce::Label::textColourId, juce::Colours::orange);

    for (auto* id : kIds)
    {
        auto k = std::make_unique<Knob>();
        k->label.setText(proc.apvts.getParameter(id)->getName(40), juce::dontSendNotification);
        k->label.setJustificationType(juce::Justification::centred);
        k->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
        k->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, id, k->slider);
        addAndMakeVisible(k->slider);
        addAndMakeVisible(k->label);
        knobs.push_back(std::move(k));
    }
    refreshLabels();
    setSize(900, 340);
}

void ToneForgeEditor::pick(bool isModel)
{
    const auto dir = juce::File("E:/Dev/ToneForge").getChildFile(isModel ? "models" : "irs");
    chooser = std::make_unique<juce::FileChooser>(isModel ? "Capture NAM" : "IR de baffle", dir,
                                                  isModel ? "*.nam" : "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, isModel](const juce::FileChooser& fc)
                         {
                             const auto f = fc.getResult();
                             if (f == juce::File())
                                 return;
                             const auto err = isModel ? proc.loadModel(f) : proc.loadIR(f);
                             if (err.isNotEmpty())
                                 juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                        "ToneForge", err);
                             refreshLabels();
                         });
}

void ToneForgeEditor::refreshLabels()
{
    auto name = [](const juce::File& f) { return f == juce::File() ? juce::String("(aucune)") : f.getFileName(); };
    modelLbl.setText("Ampli : " + name(proc.modelFile), juce::dontSendNotification);
    irLbl.setText("Baffle : " + name(proc.irFile), juce::dontSendNotification);
    warnLbl.setText(proc.modelWarning, juce::dontSendNotification);
}

void ToneForgeEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1c1c22));
    g.setColour(juce::Colours::white);
    g.setFont(22.f);
    g.drawText("ToneForge", 16, 8, 200, 30, juce::Justification::left);
}

void ToneForgeEditor::resized()
{
    auto r = getLocalBounds().reduced(16);
    r.removeFromTop(36);
    auto row = r.removeFromTop(28);
    modelBtn.setBounds(row.removeFromLeft(140));
    row.removeFromLeft(8);
    modelLbl.setBounds(row.removeFromLeft(280));
    irBtn.setBounds(row.removeFromLeft(120));
    row.removeFromLeft(8);
    irLbl.setBounds(row);
    warnLbl.setBounds(r.removeFromTop(24));
    r.removeFromTop(12);
    const int w = r.getWidth() / (int) knobs.size();
    for (auto& k : knobs)
    {
        auto c = r.removeFromLeft(w);
        k->label.setBounds(c.removeFromTop(20));
        k->slider.setBounds(c.removeFromTop(110));
    }
}
