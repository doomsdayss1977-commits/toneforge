#include "PluginEditor.h"
#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

// Bibliothèque = premier dossier parent du programme qui contient models/ ; sinon %APPDATA%\ToneForge.
static juce::File findLibRoot()
{
    for (auto d = juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
         !d.isRoot(); d = d.getParentDirectory())
        if (d.getChildFile("models").isDirectory())
            return d;
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("ToneForge");
}
static const juce::File kRoot = findLibRoot();
static juce::String u8(const char* s) { return juce::String(juce::CharPointer_UTF8(s)); }

//==============================================================================
TfLook::TfLook()
{
    using namespace tf;
    setColour(juce::ResizableWindow::backgroundColourId, bg);
    setColour(juce::Label::textColourId, txt);
    setColour(juce::Slider::textBoxTextColourId, txt);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::backgroundColourId, bg);
    setColour(juce::TextEditor::outlineColourId, line);
    setColour(juce::TextEditor::focusedOutlineColourId, acc);
    setColour(juce::TextEditor::textColourId, txt);
    setColour(juce::TextButton::buttonColourId, panel2);
    setColour(juce::TextButton::buttonOnColourId, acc2);
    setColour(juce::TextButton::textColourOffId, txt);
    setColour(juce::TextButton::textColourOnId, juce::Colour(0xff111111));
    setColour(juce::ListBox::backgroundColourId, panel);
    setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, line);
    setColour(juce::ToggleButton::textColourId, txt);
}

void TfLook::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&)
{
    const auto r = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(6);
    const float d = juce::jmin(r.getWidth(), r.getHeight()), rad = d * 0.5f - 4.f;
    const auto c = r.getCentre();
    const float a = a0 + pos * (a1 - a0);
    juce::Path bgArc, valArc;
    bgArc.addCentredArc(c.x, c.y, rad, rad, 0.f, a0, a1, true);
    valArc.addCentredArc(c.x, c.y, rad, rad, 0.f, a0, a, true);
    const juce::PathStrokeType st(6.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    g.setColour(tf::line);
    g.strokePath(bgArc, st);
    g.setColour(tf::acc);
    g.strokePath(valArc, st);
    g.setColour(tf::panel2);
    g.fillEllipse(juce::Rectangle<float>(rad * 1.3f, rad * 1.3f).withCentre(c));
    g.setColour(tf::txt);
    const auto tip = c.getPointOnCircumference(rad * 0.55f, a);
    g.drawLine({ c.getPointOnCircumference(rad * 0.15f, a), tip }, 3.f);
}

void TfLook::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool, bool)
{
    // Interrupteur « pilule » comme les .sw d'AmpliTube UI
    const auto pill = juce::Rectangle<float>(2.f, (b.getHeight() - 24) * 0.5f, 46.f, 24.f);
    const bool on = b.getToggleState();
    g.setColour(on ? tf::acc : tf::line);
    g.fillRoundedRectangle(pill, 12.f);
    g.setColour(on ? juce::Colours::white : juce::Colour(0xff888888));
    g.fillEllipse(pill.getX() + (on ? 25.f : 3.f), pill.getY() + 3.f, 18.f, 18.f);
    if (b.getButtonText().isNotEmpty())
    {
        g.setColour(tf::txt);
        g.setFont(14.f);
        g.drawText(b.getButtonText(), b.getLocalBounds().withTrimmedLeft(56), juce::Justification::centredLeft);
    }
}

void TfLook::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& fill, bool hover, bool)
{
    const auto r = b.getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(b.getToggleState() ? findColour(juce::TextButton::buttonOnColourId) : fill);
    g.fillRoundedRectangle(r, 8.f);
    g.setColour(hover ? tf::acc : tf::line);
    g.drawRoundedRectangle(r, 8.f, 1.f);
}

//==============================================================================
ToneForgeEditor::ToneForgeEditor(ToneForgeProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    setLookAndFeel(&look);

    // Chaîne dans l'ordre du signal
    auto fxBlock = [](const FxDef& d, const juce::String& cat)
    {
        const auto t = u8(d.title);
        BlockDef b { t.upToFirstOccurrenceOf(" (", false, false), cat, juce::String(d.key) + "_on",
                     t.fromFirstOccurrenceOf("(", false, false).upToLastOccurrenceOf(")", false, false) + " (Airwindows)" };
        for (size_t i = 0; i < d.params.size(); ++i)
        {
            b.ids.add(juce::String(d.key) + "_" + juce::String((int) i));
            b.labels.add(u8(d.params[i].label));
        }
        return b;
    };
    const auto& fx = fxDefs();
    const juce::StringArray cats { "Dynamique", "Modulation", u8("Écho"), "Espace" };
    defs.add({ "Gate", "Bruit", "gate_on", "Coupe le souffle sous le seuil", { "gate" }, { "Seuil (dB)" } });
    defs.add(fxBlock(fx[0], cats[0]));
    defs.add({ "Ampli", "NAM", {}, {}, { "input", "output" }, { u8("Entrée (dB)"), "Sortie (dB)" } });
    defs.add({ "Baffle", "IR", "ir_on", {}, {}, {} });
    defs.add({ "EQ", u8("Tonalité"), {}, "Graves 120 Hz, Médiums 800 Hz, Aigus 3,5 kHz", { "bass", "mid", "treble" },
               { "Graves", u8("Médiums"), "Aigus" } });
    for (size_t i = 1; i < fx.size(); ++i)
        defs.add(fxBlock(fx[i], cats[(int) i]));
    for (int i = 0; i < defs.size(); ++i)
    {
        blocks.push_back(std::make_unique<Block>(*this, i));
        addAndMakeVisible(*blocks.back());
    }

    // Navigateur
    search.setTextToShowWhenEmpty("Rechercher une capture...", tf::dim);
    search.onTextChange = [this] { filterList(); };
    addAndMakeVisible(search);
    for (auto* b : { &ampTab, &cabTab })
    {
        b->setClickingTogglesState(false);
        addAndMakeVisible(b);
    }
    ampTab.onClick = [this] { showModels = true; filterList(); };
    cabTab.onClick = [this] { showModels = false; filterList(); };
    rescanBtn.onClick = [this] { scanLibrary(); };
    fileBtn.onClick = [this] { pick(); };
    addAndMakeVisible(rescanBtn);
    addAndMakeVisible(fileBtn);
    countLbl.setColour(juce::Label::textColourId, tf::dim);
    countLbl.setFont(12.f);
    addAndMakeVisible(countLbl);
    list.setRowHeight(52);
    addAndMakeVisible(list);

    // Barre du haut et panneau
    nameLbl.setFont(juce::FontOptions(18.f, juce::Font::bold));
    nameLbl.setJustificationType(juce::Justification::centred);
    warnLbl.setColour(juce::Label::textColourId, tf::warn);
    warnLbl.setFont(12.f);
    panelTitle.setFont(juce::FontOptions(18.f, juce::Font::bold));
    panelInfo.setColour(juce::Label::textColourId, tf::dim);
    for (auto* l : { &nameLbl, &warnLbl, &panelTitle, &panelInfo })
        addAndMakeVisible(l);
    bypass.setButtonText("Actif");
    addChildComponent(bypass);

    scanLibrary();
    proc.apvts.state.addListener(this);
    selectBlock(selected);
    refreshLabels();
    setSize(1180, 640);
    startTimerHz(30);
}

ToneForgeEditor::~ToneForgeEditor()
{
    proc.apvts.state.removeListener(this);
    stopTimer();
    setLookAndFeel(nullptr);
}

//==============================================================================
void ToneForgeEditor::scanLibrary()
{
    auto scan = [](const char* sub, const char* pattern)
    {
        juce::Array<Entry> out;
        for (const auto& f : kRoot.getChildFile(sub).findChildFiles(juce::File::findFiles, true, pattern))
            out.add({ f, f.getParentDirectory().getFileName(), f.getFileNameWithoutExtension() });
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b)
                  { return (a.group + a.name).compareNatural(b.group + b.name) < 0; });
        return out;
    };
    models = scan("models", "*.nam");
    irs = scan("irs", "*.wav");
    filterList();
}

void ToneForgeEditor::filterList()
{
    const auto q = search.getText().trim();
    shown.clearQuick();
    for (const auto& e : showModels ? models : irs)
        if (q.isEmpty() || (e.group + " " + e.name).containsIgnoreCase(q))
            shown.add(e);
    ampTab.setToggleState(showModels, juce::dontSendNotification);
    cabTab.setToggleState(!showModels, juce::dontSendNotification);
    ampTab.setButtonText("Amplis (" + juce::String(models.size()) + ")");
    cabTab.setButtonText("Baffles (" + juce::String(irs.size()) + ")");
    countLbl.setText(juce::String(shown.size()) + (showModels ? " captures" : " IR"), juce::dontSendNotification);
    list.updateContent();
    list.repaint();
}

void ToneForgeEditor::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool)
{
    if (!juce::isPositiveAndBelow(row, shown.size()))
        return;
    const auto& e = shown.getReference(row);
    const bool current = e.file == (showModels ? proc.modelFile : proc.irFile);
    const auto r = juce::Rectangle<int>(0, 0, w, h).toFloat().reduced(2.f, 3.f);
    g.setColour(tf::panel2);
    g.fillRoundedRectangle(r, 10.f);
    g.setColour(current ? tf::acc : tf::line);
    g.drawRoundedRectangle(r, 10.f, current ? 2.f : 1.f);
    auto t = r.reduced(10.f, 6.f).toNearestInt();
    g.setColour(tf::txt);
    g.setFont(juce::FontOptions(14.f, juce::Font::bold));
    g.drawText(e.name, t.removeFromTop(20), juce::Justification::centredLeft, true);
    g.setColour(showModels ? tf::acc : tf::acc2);
    g.setFont(11.f);
    g.drawText(e.group, t, juce::Justification::centredLeft, true);
}

void ToneForgeEditor::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow(row, shown.size()))
        load(shown[row].file, showModels);
}

void ToneForgeEditor::load(const juce::File& f, bool isModel)
{
    if (f == (isModel ? proc.modelFile : proc.irFile))
    {
        selectBlock(isModel ? 2 : 3);   // déjà chargé : pas de rechargement
        return;
    }
    const auto err = isModel ? proc.loadModel(f) : proc.loadIR(f);
    dirty = true;
    if (err.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "ToneForge", err);
    selectBlock(isModel ? 2 : 3);
    refreshLabels();
    list.repaint();
}

void ToneForgeEditor::pick()
{
    const bool isModel = showModels;
    chooser = std::make_unique<juce::FileChooser>(isModel ? "Capture NAM" : "IR de baffle",
                                                  kRoot.getChildFile(isModel ? "models" : "irs"),
                                                  isModel ? "*.nam" : "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, isModel](const juce::FileChooser& fc)
                         {
                             if (fc.getResult() != juce::File())
                                 load(fc.getResult(), isModel);
                         });
}

void ToneForgeEditor::refreshLabels()
{
    auto name = [](const juce::File& f) { return f == juce::File() ? juce::String("(aucun)") : f.getFileNameWithoutExtension(); };
    nameLbl.setText(name(proc.modelFile), juce::dontSendNotification);
    warnLbl.setText(proc.modelWarning(), juce::dontSendNotification);
    if (selected == 2)
        panelInfo.setText(proc.modelFile.getFullPathName(), juce::dontSendNotification);
    else if (selected == 3)
        panelInfo.setText(proc.irFile == juce::File() ? juce::String("Aucune IR : choisis-en une dans Baffles.")
                                                      : proc.irFile.getFullPathName(), juce::dontSendNotification);
    else
        panelInfo.setText(defs[selected].info, juce::dontSendNotification);
}

//==============================================================================
bool ToneForgeEditor::blockOn(int i) const
{
    const auto& d = defs.getReference(i);
    if (d.onId.isNotEmpty())
        return proc.apvts.getRawParameterValue(d.onId)->load() > 0.5f;
    if (i == 2)
        return proc.modelFile != juce::File();
    return true;
}

void ToneForgeEditor::toggleBlock(int i)
{
    if (auto* p = proc.apvts.getParameter(defs[i].onId))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost(blockOn(i) ? 0.f : 1.f);
        p->endChangeGesture();
    }
    for (auto& b : blocks)
        b->repaint();
}

void ToneForgeEditor::selectBlock(int i)
{
    selected = i;
    const auto& d = defs.getReference(i);
    panelTitle.setText(d.title, juce::dontSendNotification);
    bypassAtt.reset();
    bypass.setVisible(d.onId.isNotEmpty());
    if (d.onId.isNotEmpty())
        bypassAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, d.onId, bypass);
    bypass.onClick = [this] { for (auto& b : blocks) b->repaint(); };

    knobs.clear();
    for (int k = 0; k < d.ids.size(); ++k)
    {
        auto kn = std::make_unique<Knob>();
        kn->label.setText(d.labels[k], juce::dontSendNotification);
        kn->label.setJustificationType(juce::Justification::centred);
        kn->label.setColour(juce::Label::textColourId, tf::dim);
        kn->slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        kn->slider.setColour(juce::Slider::textBoxTextColourId, tf::txt);
        kn->slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 20);
        kn->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, d.ids[k], kn->slider);
        const int dec = proc.apvts.getParameterRange(d.ids[k]).end <= 1.f ? 2 : 1;
        kn->slider.textFromValueFunction = [dec](double v) { return juce::String(v, dec); };
        kn->slider.updateText();
        addAndMakeVisible(kn->slider);
        addAndMakeVisible(kn->label);
        knobs.push_back(std::move(kn));
    }
    refreshLabels();
    for (auto& b : blocks)
        b->repaint();
    resized();
}

void ToneForgeEditor::Block::paint(juce::Graphics& g)
{
    const auto& d = ed.defs.getReference(index);
    const bool on = ed.blockOn(index), sel = ed.selected == index;
    const auto r = getLocalBounds().toFloat().reduced(2.f);
    g.setColour(tf::panel);
    g.fillRoundedRectangle(r, 10.f);
    g.setColour(sel ? tf::acc : index == 2 ? juce::Colour(0xff5a3a22) : index == 3 ? juce::Colour(0xff1d3f52) : tf::line);
    g.drawRoundedRectangle(r, 10.f, sel ? 2.f : 1.f);
    g.setOpacity(on ? 1.f : 0.45f);
    auto t = r.reduced(10.f, 8.f).toNearestInt();
    g.setColour(tf::dim);
    g.setFont(10.f);
    g.drawText(d.kind.toUpperCase(), t.removeFromTop(16).withTrimmedRight(14), juce::Justification::centredLeft, true);
    g.setColour(tf::txt);
    g.setFont(juce::FontOptions(15.f, juce::Font::bold));
    g.drawText(d.title, t, juce::Justification::centredLeft, true);
    // LED
    const auto led = juce::Rectangle<float>(r.getRight() - 18.f, r.getY() + 8.f, 10.f, 10.f);
    g.setOpacity(1.f);
    if (on)
    {
        g.setColour(tf::ok.withAlpha(0.35f));
        g.fillEllipse(led.expanded(3.f));
    }
    g.setColour(on ? tf::ok : juce::Colour(0xff444444));
    g.fillEllipse(led);
}

void ToneForgeEditor::Block::mouseUp(const juce::MouseEvent& e)
{
    // Clic sur la LED = marche/arrêt ; ailleurs = afficher les réglages du bloc.
    if (ed.defs[index].onId.isNotEmpty() && e.x > getWidth() - 30 && e.y < 30)
        ed.toggleBlock(index);
    else
        ed.selectBlock(index);
}

//==============================================================================
void ToneForgeEditor::timerCallback()
{
    auto fall = [](float& lvl, std::atomic<float>& pk) { lvl = juce::jmax(pk.exchange(0.f), lvl * 0.85f); };
    fall(levelIn, proc.inPeak);
    fall(levelOut, proc.outPeak);
    repaint(meterIn.getUnion(meterOut).expanded(4, 16));

    // Toutes les 2 s : LED à jour et réglages sauvegardés s'ils ont changé (l'appli autonome ne sauve sinon qu'à la fermeture).
    if (++ticks % 60 == 0)
    {
        for (auto& b : blocks)
            b->repaint();
        warnLbl.setText(proc.modelWarning(), juce::dontSendNotification);   // le taux du projet a pu changer
       #if JucePlugin_Build_Standalone
        if (auto* h = juce::StandalonePluginHolder::getInstance(); h && std::exchange(dirty, false))
            h->savePluginState();
       #endif
    }
}

void ToneForgeEditor::paint(juce::Graphics& g)
{
    g.fillAll(tf::bg);
    g.setColour(tf::panel);
    g.fillRect(topBar);
    g.fillRect(0, topBar.getBottom(), 260, getHeight());
    g.setColour(tf::line);
    g.drawHorizontalLine(topBar.getBottom(), 0.f, (float) getWidth());
    g.drawVerticalLine(260, (float) topBar.getBottom(), (float) getHeight());

    g.setColour(tf::acc);
    g.setFont(juce::FontOptions(20.f, juce::Font::bold));
    g.drawText("ToneForge", 16, 0, 200, topBar.getHeight(), juce::Justification::centredLeft);

    // Flèches entre les blocs
    g.setColour(tf::dim);
    for (size_t i = 0; i + 1 < blocks.size(); ++i)
    {
        const auto a = blocks[i]->getBounds(), b = blocks[i + 1]->getBounds();
        g.drawText(juce::String::fromUTF8("\xe2\x80\xba"), a.getRight(), a.getY(), b.getX() - a.getRight(), a.getHeight(),
                   juce::Justification::centred);
    }

    g.setColour(tf::panel);
    g.fillRoundedRectangle(panelArea.toFloat(), 10.f);
    g.setColour(tf::line);
    g.drawRoundedRectangle(panelArea.toFloat(), 10.f, 1.f);

    // Vumètres
    auto meter = [&g](juce::Rectangle<int> m, float lvl, const char* lbl)
    {
        g.setColour(juce::Colour(0xff0a0c10));
        g.fillRoundedRectangle(m.toFloat(), 3.f);
        const float db = juce::Decibels::gainToDecibels(lvl, -60.f);
        const int hh = (int) (m.getHeight() * (db + 60.f) / 60.f);
        g.setColour(db > -3.f ? tf::bad : db > -12.f ? tf::warn : tf::ok);
        g.fillRoundedRectangle(m.withTop(m.getBottom() - hh).toFloat(), 3.f);
        g.setColour(tf::dim);
        g.setFont(10.f);
        g.drawText(lbl, m.getX() - 10, m.getBottom() + 1, m.getWidth() + 20, 12, juce::Justification::centred);
    };
    meter(meterIn, levelIn, "IN");
    meter(meterOut, levelOut, "OUT");
}

void ToneForgeEditor::resized()
{
    auto r = getLocalBounds();
    topBar = r.removeFromTop(56);
    {
        auto t = topBar.reduced(16, 0);
        meterOut = t.removeFromRight(10).withSizeKeepingCentre(10, 34).translated(0, -5);
        t.removeFromRight(14);
        meterIn = t.removeFromRight(10).withSizeKeepingCentre(10, 34).translated(0, -5);
        t.removeFromRight(16);
        warnLbl.setBounds(t.removeFromRight(260));
        nameLbl.setBounds(t.withTrimmedLeft(180));
    }

    auto side = r.removeFromLeft(260).reduced(12);
    search.setBounds(side.removeFromTop(34));
    side.removeFromTop(8);
    auto tabs = side.removeFromTop(28);
    const int tw = tabs.getWidth() / 2 - 3;
    ampTab.setBounds(tabs.removeFromLeft(tw));
    cabTab.setBounds(tabs.removeFromRight(tw));
    side.removeFromTop(6);
    auto tools = side.removeFromTop(26);
    countLbl.setBounds(tools.removeFromLeft(80));
    fileBtn.setBounds(tools.removeFromRight(66));
    tools.removeFromRight(6);
    rescanBtn.setBounds(tools.removeFromRight(84));
    side.removeFromTop(6);
    list.setBounds(side);

    auto c = r.reduced(16);
    chainArea = c.removeFromTop(70);
    {
        const int n = (int) blocks.size(), gap = 12;
        const int bw = (chainArea.getWidth() - gap * (n - 1)) / n;
        auto row = chainArea;
        for (auto& b : blocks)
        {
            b->setBounds(row.removeFromLeft(bw));
            row.removeFromLeft(gap);
        }
    }
    c.removeFromTop(16);
    panelArea = c;
    auto p = panelArea.reduced(16);
    auto head = p.removeFromTop(30);
    panelTitle.setBounds(head.removeFromLeft(200));
    bypass.setBounds(head.removeFromLeft(140));
    panelInfo.setBounds(p.removeFromTop(22));
    p.removeFromTop(16);
    const int kw = 110;
    for (auto& k : knobs)
    {
        auto col = p.withWidth(kw);
        p.removeFromLeft(kw + 18);
        k->label.setBounds(col.removeFromTop(20));
        k->slider.setBounds(col.removeFromTop(120));
    }
}
