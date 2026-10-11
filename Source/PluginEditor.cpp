#include "PluginEditor.h"

static const char* kIds[] = { "gate", "input", "bass", "mid", "treble", "output" };

ToneForgeEditor::ToneForgeEditor(ToneForgeProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    for (auto* b : { &modelBtn, &irBtn })
        addAndMakeVisible(b);
    modelBtn.onClick = [this] { pick(true); };
    irBtn.onClick = [this] { pick(false); };
    for (auto* l : { &modelLbl, &irLbl, &warnLbl })
        addAndMakeVisible(l);
    addAndMakeVisible(irOn);
    irOnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, "ir_on", irOn);
    warnLbl.setColour(juce::Label::textColourId, juce::Colours::orange);

    for (auto* id : kIds)
        knobs.push_back(makeKnob(id, proc.apvts.getParameter(id)->getName(40)));

    for (auto& d : fxDefs())
    {
        auto row = std::make_unique<FxRow>();
        const juce::String key(d.key);
        row->on.setButtonText(juce::String(juce::CharPointer_UTF8(d.title)));
        row->att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, key + "_on", row->on);
        addAndMakeVisible(row->on);
        for (size_t i = 0; i < d.params.size(); ++i)
            row->knobs.push_back(makeKnob(key + "_" + juce::String((int) i),
                                          juce::String(juce::CharPointer_UTF8(d.params[i].label))));
        rows.push_back(std::move(row));
    }
    refreshLabels();
    setSize(900, 280 + 120 * (int) rows.size());
}

std::unique_ptr<ToneForgeEditor::Knob> ToneForgeEditor::makeKnob(const juce::String& id, const juce::String& label)
{
    auto k = std::make_unique<Knob>();
    k->label.setText(label, juce::dontSendNotification);
    k->label.setJustificationType(juce::Justification::centred);
    k->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
    k->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, id, k->slider);
    addAndMakeVisible(k->slider);
    addAndMakeVisible(k->label);
    return k;
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
    irOn.setBounds(row.removeFromLeft(70));
    irLbl.setBounds(row);
    warnLbl.setBounds(r.removeFromTop(24));
    r.removeFromTop(12);
    auto layoutKnobs = [](std::vector<std::unique_ptr<Knob>>& ks, juce::Rectangle<int> area, int h)
    {
        const int w = area.getWidth() / 6;   // largeur fixe : les rangées restent alignées
        for (auto& k : ks)
        {
            auto c = area.removeFromLeft(w);
            k->label.setBounds(c.removeFromTop(18));
            k->slider.setBounds(c.removeFromTop(h));
        }
    };
    layoutKnobs(knobs, r.removeFromTop(130), 110);
    for (auto& row : rows)
    {
        r.removeFromTop(8);
        auto band = r.removeFromTop(112);
        row->on.setBounds(band.removeFromLeft(170).withSizeKeepingCentre(170, 28));
        layoutKnobs(row->knobs, band, 92);
    }
}
