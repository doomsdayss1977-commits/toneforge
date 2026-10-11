#pragma once
#include "PluginProcessor.h"

// Thème repris d'AmpliTube UI (palette sombre, accent orange / cyan).
namespace tf
{
const juce::Colour bg { 0xff0f1115 }, panel { 0xff171a21 }, panel2 { 0xff1e222b }, line { 0xff2a2f3a },
    txt { 0xffe6e8ec }, dim { 0xff8a91a0 }, acc { 0xffff8a3d }, acc2 { 0xff3dd6ff }, ok { 0xff3ddc84 },
    warn { 0xffffcc3d }, bad { 0xffff4d5e };
}

class TfLook : public juce::LookAndFeel_V4
{
public:
    TfLook();
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool hover, bool down) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool hover, bool down) override;
};

class ToneForgeEditor : public juce::AudioProcessorEditor, private juce::ListBoxModel, private juce::Timer
{
public:
    explicit ToneForgeEditor(ToneForgeProcessor&);
    ~ToneForgeEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Bloc de la chaîne : titre, type, interrupteur éventuel, réglages.
    struct BlockDef
    {
        juce::String title, kind, onId, info;   // kind = catégorie affichée sur le bloc
        juce::StringArray ids, labels;
    };
    struct Block : juce::Component
    {
        ToneForgeEditor& ed;
        int index;
        Block(ToneForgeEditor& e, int i) : ed(e), index(i) {}
        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent&) override;
    };
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    struct Entry
    {
        juce::File file;
        juce::String group, name;
    };

    // Navigateur de captures (ListBoxModel)
    int getNumRows() override { return shown.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;

    void timerCallback() override;
    void scanLibrary();
    void filterList();
    void selectBlock(int i);
    bool blockOn(int i) const;
    void toggleBlock(int i);
    void load(const juce::File&, bool isModel);
    void pick();
    void refreshLabels();

    ToneForgeProcessor& proc;
    TfLook look;
    juce::Array<BlockDef> defs;
    std::vector<std::unique_ptr<Block>> blocks;
    int selected = 2;   // Ampli

    juce::TextEditor search;
    juce::TextButton ampTab { "Amplis" }, cabTab { "Baffles" }, rescanBtn { "Actualiser" }, fileBtn { "Fichier..." };
    juce::Label countLbl, nameLbl, warnLbl, panelTitle, panelInfo;
    juce::ListBox list { "captures", this };
    juce::Array<Entry> models, irs, shown;
    bool showModels = true;

    juce::ToggleButton bypass;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAtt;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> topBar, chainArea, panelArea, meterIn, meterOut;
    float levelIn = 0.f, levelOut = 0.f;
    int ticks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneForgeEditor)
};
