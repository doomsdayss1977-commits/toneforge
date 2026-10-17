#pragma once
#include "PluginProcessor.h"
#include "AiTone.h"
#include "Tuner.h"

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
    juce::Font getTextButtonFont(juce::TextButton&, int h) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool hover, bool down) override;
    juce::Slider::SliderLayout getSliderLayout(juce::Slider&) override;   // molette : valeur éditable au centre
    juce::Label* createSliderTextBox(juce::Slider&) override;   // propriété « icon » : icône vectorielle + texte
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
        float hov = 0.f, selA = 0.f, onA = 1.f;   // animations (~150 ms) : survol, sélection, marche
        Block(ToneForgeEditor& e, int i) : ed(e), index(i) { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
        void paint(juce::Graphics&) override;
        void mouseUp(const juce::MouseEvent&) override;
    };
    struct RightClick : juce::MouseListener
    {
        std::function<void()> fn;
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && fn)
                fn();
        }
    };
    // Molette que le clic droit ne fait pas tourner (il ouvre le menu macro).
    struct KnobSlider : juce::Slider
    {
        using juce::Slider::Slider;
        void mouseDown(const juce::MouseEvent& e) override { if (!e.mods.isPopupMenu()) juce::Slider::mouseDown(e); }
        void mouseDrag(const juce::MouseEvent& e) override { if (!e.mods.isPopupMenu()) juce::Slider::mouseDrag(e); }
        void mouseUp(const juce::MouseEvent& e) override { if (!e.mods.isPopupMenu()) juce::Slider::mouseUp(e); }
    };
    struct Knob
    {
        RightClick rc;   // en premier : détruit après la molette qui l'écoute
        KnobSlider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    struct Entry
    {
        juce::File file;
        juce::String group, name, brand;   // fichier vide = en-tête de marque dans la liste
        juce::String key;   // clé des étiquettes (chemin relatif), calculée une fois au balayage
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
    // Bibliothèques : 0 = amplis, 1 = pédales, 2 = baffles (IR)
    const juce::Array<Entry>& lib() const { return tab == 0 ? models : tab == 1 ? pedals : irs; }
    // 3 = ampli B, 4 = IR B : les onglets Amplis/Baffles chargent dans B quand son bloc est affiché
    juce::File current(int kind) const { return proc.loadedFile(kind); }
    static int blockOf(int kind) { return kind == 0 ? kAmp : kind == 1 ? kPedal : kind == 2 ? kCab : kAmpB; }
    int target(int t) const { return selected == kAmpB && t != 1 ? (t == 0 ? 3 : 4) : t; }
    void setTab(int t);
    void load(const juce::File&, int kind);
    void pick();
    void refreshLabels();
    void renderBackground();
    void askAi();
    void applyAi(const juce::String& reply, const juce::String& err);
    // Réglages par bloc (copier/coller, presets .tfb) et tap tempo du delay
    juce::String blockKey(int i) const;
    juce::ValueTree captureBlock(int i) const;
    void applyBlock(int i, const juce::ValueTree&);
    void blockMenu(int i);
    void tap();
    void showTuner(bool on);
    void updateTuner();
    // Étiquettes des captures (tags.json de la bibliothèque) : genre, caractère, etc.
    juce::StringArray tagsOf(const juce::String& key) const;
    void saveTags() const;
    void tagMenu(int row);

    ToneForgeProcessor& proc;
    TfLook look;
    juce::Array<BlockDef> defs;
    std::vector<std::unique_ptr<Block>> blocks;
    enum { kPedal = 2, kAmp = 3, kCab = 4, kAmpB = 5 };   // blocs NAM et IR dans la chaîne
    int selected = kAmp;

    juce::TextEditor search;
    juce::TextButton ampTab { "Amplis" }, pedTab { u8("Pédales") }, cabTab { "Baffles" }, rescanBtn { "Actualiser" }, fileBtn { "Fichier..." };
    juce::Label countLbl, nameLbl, warnLbl, panelTitle, panelInfo;
    juce::ListBox list { "captures", this };
    juce::Array<Entry> models, pedals, irs, shown;
    int tab = 0;
    juce::OwnedArray<juce::TextButton> brandBtns;   // filtre par marque (« Toutes » en premier)
    juce::String brandFilter;
    std::map<juce::String, juce::StringArray> tagMap;   // chemin relatif à la bibliothèque -> étiquettes

    juce::ToggleButton bypass;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAtt;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TextButton presetBtn { u8("Réglages ▾") }, tapBtn { "Tap" };
    juce::ValueTree clip;          // réglages copiés (type = clé du bloc)
    juce::Array<double> taps;      // instants des derniers coups de Tap (ms)
    juce::TextButton tunerBtn { "Accordeur" };
    std::vector<float> tunerBuf;
    juce::TextButton loopBtn, loopStopBtn, metroBtn;   // looper + métronome (haut de la grille)
    void updateLooper();
    juce::Slider macroSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label macroLbl;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> macroAtt;
    void macroMenu(const juce::String& id);

    // Mode « Son IA » : demande en texte libre -> capture, IR et réglages choisis par OpenRouter
    juce::TextButton aiBtn, aiGo, aiClose { "Fermer" };
    juce::TextEditor aiPrompt, aiOut;
    bool aiOpen = false, aiBusy = false;
    std::unique_ptr<ai::Job> aiJob;

    juce::Rectangle<int> topBar, chainArea, panelArea, plateArea, grilleArea, lcdArea, meterIn, meterOut;
    juce::Image grain, bgCache;   // texture tolex ; décor statique pré-rendu (refait au changement de bloc)
    float levelIn = 0.f, levelOut = 0.f, needleIn = -60.f, needleOut = -60.f;   // niveaux crête des vumètres en dB
    int ticks = 0;
    int hoverRow = -1;   // ligne de la liste sous la souris (suivie par le timer)
    float holdIn = -60.f, holdOut = -60.f;   // crêtes maintenues des vumètres LED (dB)
    int holdInT = 0, holdOutT = 0;
    float pulse = 0.f;   // position (0-1) de l'impulsion qui parcourt le câble de la chaîne
    juce::Rectangle<int> cableStrip() const;
    int seenLoad = -1;
    bool dirty = false;   // réglages à sauvegarder

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ToneForgeEditor)
};
