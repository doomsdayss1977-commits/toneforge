#pragma once
#include "PluginEditor.h"

// Interface iPhone (paysage) : chaîne en pastilles, une vue par bloc avec de grosses barres,
// liste de captures et accordeur en plein écran. Sur PC : variable d'environnement TF_PHONE=1.
class PhoneEditor : public juce::AudioProcessorEditor, private juce::ListBoxModel, private juce::Timer,
                    private juce::ValueTree::Listener
{
public:
    explicit PhoneEditor(ToneForgeProcessor&);
    ~PhoneEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Look : TfLook
    {
        juce::Font getTextButtonFont(juce::TextButton&, int h) override { return juce::Font(juce::FontOptions(juce::jlimit(14.f, 20.f, h * 0.36f), juce::Font::bold)); }
        juce::Label* createSliderTextBox(juce::Slider& s) override
        {
            auto* l = TfLook::createSliderTextBox(s);
            l->setFont(juce::FontOptions(18.f, juce::Font::bold));
            return l;
        }
    };
    // Bloc : unité de la chaîne (« amp » = cœur fixe), interrupteur, réglages, genres chargeables (cf. loadedFile)
    struct Def
    {
        juce::String title, unit, onId;
        juce::StringArray ids, labels;
        juce::Array<int> kinds;
    };
    struct Row { juce::File file; juce::String brand, name; };   // fichier vide = en-tête de marque
    struct Param
    {
        juce::Label label;
        juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    enum Mode { main, browse, tune };

    int getNumRows() override { return rows.size(); }
    void paintListBoxItem(int row, juce::Graphics&, int w, int h, bool) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void timerCallback() override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { dirty = true; }

    void rebuildChain();
    void select(int d);
    bool isOn(int d) const;
    void refreshChips();
    void refreshName();
    void setMode(Mode);
    void openBrowser(int kind);
    void showRows();
    void load(const juce::File&);
    void updateTuner();

    ToneForgeProcessor& proc;
    Look look;
    juce::Array<Def> defs;
    std::vector<int> order;
    juce::StringArray shownChain;
    int sel = -1;
    Mode mode = main;

    juce::Viewport chipView;
    juce::Component chipBox;
    juce::OwnedArray<juce::TextButton> chips;
    juce::Label title, capName;
    juce::TextButton onBtn;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAtt;
    juce::OwnedArray<juce::TextButton> pickBtns;
    juce::Viewport paramView;
    juce::Component paramBox;
    std::vector<std::unique_ptr<Param>> params;
    juce::TextButton tunerBtn { "Accordeur" };
    juce::Label volLbl;
    juce::Slider volume { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volAtt;
    juce::Rectangle<int> meterArea;
    float lvlIn = 0.f, lvlOut = 0.f;

    // Liste plein écran
    juce::Label browseTitle;
    juce::TextButton closeBtn { "Fermer" };
    juce::Viewport brandView;
    juce::Component brandBox;
    juce::OwnedArray<juce::TextButton> brandBtns;
    juce::ListBox list { "captures", this };
    juce::Array<Row> all, rows;
    juce::String brand;
    int browseKind = 0;

    // Accordeur plein écran
    std::vector<float> tunerBuf;
    juce::String note { "--" };
    int cents = 0;
    float hz = 0.f;
    bool heard = false;
    juce::Rectangle<int> tuneArea;

    int ticks = 0, seenLoad = -1;
    bool dirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhoneEditor)
};
