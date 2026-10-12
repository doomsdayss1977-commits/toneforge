#pragma once
#include "PluginProcessor.h"
#include "AiTone.h"

// Thème repris d'AmpliTube UI (palette sombre, accent orange / cyan).
namespace tf
{
const juce::Colour bg { 0xff0f1115 }, panel { 0xff171a21 }, panel2 { 0xff1e222b }, line { 0xff2a2f3a },
    txt { 0xffe6e8ec }, dim { 0xff8a91a0 }, acc { 0xffff8a3d }, acc2 { 0xff3dd6ff }, ok { 0xff3ddc84 },
    warn { 0xffffcc3d }, bad { 0xffff4d5e }, ink { 0xff1c1d20 }, cream { 0xfff1e6c8 }, amber { 0xffffb347 };
}

class TfLook : public juce::LookAndFeel_V4
{
public:
    TfLook();
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool hover, bool down) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool hover, bool down) override;
    void fillTextEditorBackground(juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int w, int h, juce::TextEditor&) override;
};

class ToneForgeEditor : public juce::AudioProcessorEditor, private juce::ListBoxModel, private juce::Timer,
                        private juce::ValueTree::Listener
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
        Block(ToneForgeEditor& e, int i) : ed(e), index(i) { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
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
        juce::String group, name, brand;   // fichier vide = en-tête de marque dans la liste
    };

    // Navigateur de captures (ListBoxModel)
    int getNumRows() override { return shown.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;

    void timerCallback() override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { dirty = true; }
    void scanLibrary();
    void filterList();
    void rebuildBrands();
    void selectBlock(int i);
    bool blockOn(int i) const;
    void toggleBlock(int i);
    void load(const juce::File&, bool isModel);
    void pick();
    void refreshLabels();
    void renderBackground();
    void askAi();
    void applyAi(const juce::String& reply, const juce::String& err);

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
    juce::OwnedArray<juce::TextButton> brandBtns;   // filtre par marque (« Toutes » en premier)
    juce::String brandFilter;

    juce::ToggleButton bypass;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAtt;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::unique_ptr<juce::FileChooser> chooser;

    // Mode « Son IA » : demande en texte libre -> capture, IR et réglages choisis par OpenRouter
    juce::TextButton aiBtn, aiGo, aiClose { "Fermer" };
    juce::TextEditor aiPrompt, aiOut;
    bool aiOpen = false, aiBusy = false;
    std::unique_ptr<ai::Job> aiJob;

    juce::Rectangle<int> topBar, chainArea, panelArea, plateArea, grilleArea, lcdArea, meterIn, meterOut;
    juce::Image grain, bgCache;   // texture tolex ; décor statique pré-rendu (refait au changement de bloc)
    float levelIn = 0.f, levelOut = 0.f, needleIn = -60.f, needleOut = -60.f;   // aiguilles VU en dB
    int ticks = 0;
    bool dirty = false;   // réglages à sauvegarder

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneForgeEditor)
};
