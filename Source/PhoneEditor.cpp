#include "PhoneEditor.h"
#if JucePlugin_Build_Standalone
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

PhoneEditor::PhoneEditor(ToneForgeProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    setLookAndFeel(&look);
    look.setColour(juce::Slider::trackColourId, tf::acc);
    look.setColour(juce::Slider::thumbColourId, tf::txt);
    look.setColour(juce::Slider::backgroundColourId, tf::line);
    look.setColour(juce::Slider::textBoxTextColourId, tf::txt);
    look.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    look.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    // Blocs : Gate, effets, pédales, puis le cœur fixe (unité « amp ») ; l'ordre affiché suit la chaîne du processeur.
    const juce::StringArray pedLabels { u8("Entrée (dB)"), "Sortie (dB)", u8("Tonalité"), u8("Mélange") };
    defs.add({ "Gate", "gate", "gate_on", { "gate" }, { "Seuil (dB)" }, {} });
    for (const auto& d : fxDefs())
    {
        Def b { u8(d.title).upToFirstOccurrenceOf(" (", false, false), d.key, juce::String(d.key) + "_on", {}, {}, {} };
        for (size_t i = 0; i < d.params.size(); ++i)
        {
            b.ids.add(juce::String(d.key) + "_" + juce::String((int) i));
            b.labels.add(u8(d.params[i].label));
        }
        defs.add(b);
    }
    for (int s = 0; s < ToneForgeProcessor::kPedals; ++s)
    {
        const auto id = ToneForgeProcessor::pedalId(s);
        defs.add({ s == 0 ? u8("Pédale") : u8("Pédale ") + juce::String(s + 1), id, id + "_on",
                   { id + "_in", id + "_out", id + "_tone", id + "_mix" }, pedLabels, { ToneForgeProcessor::pedalKind(s) } });
    }
    defs.add({ "Ampli", "amp", {}, { "input", "output" }, { u8("Entrée (dB)"), "Sortie (dB)" }, { 0 } });
    defs.add({ "Baffle", "amp", "ir_on", {}, {}, { 2 } });
    defs.add({ "Ampli B", "amp", "ampb_on", { "ampb_in", "ampb_out", "ab_mix", "dyn_amt", "dyn_thr", "dyn_range", "dyn_rel" },
               { u8("Entrée (dB)"), "Sortie (dB)", u8("Mélange A/B"), "Attaque", "Seuil (dB)", "Plage (dB)", "Retour (ms)" }, { 3, 4 } });
    defs.add({ "EQ", "amp", {}, { "bass", "mid", "treble" }, { "Graves (dB)", u8("Médiums (dB)"), "Aigus (dB)" }, {} });

    chipView.setViewedComponent(&chipBox, false);
    chipView.setScrollBarsShown(false, false, false, true);
    addAndMakeVisible(chipView);

    title.setFont(juce::FontOptions(28.f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, tf::txt);
    capName.setFont(juce::FontOptions(17.f, juce::Font::bold));
    capName.setColour(juce::Label::textColourId, tf::amber);
    capName.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(title);
    addAndMakeVisible(capName);
    onBtn.setClickingTogglesState(true);
    onBtn.onStateChange = [this] { onBtn.setButtonText(onBtn.getToggleState() ? "MARCHE" : u8("ARRÊT")); };
    onBtn.onClick = [this] { refreshChips(); };
    addChildComponent(onBtn);
    paramView.setViewedComponent(&paramBox, false);
    paramView.setScrollBarsShown(false, false, true, false);
    addAndMakeVisible(paramView);

    tunerBtn.onClick = [this] { setMode(tune); };
    addAndMakeVisible(tunerBtn);
    volLbl.setText("Volume", juce::dontSendNotification);
    volLbl.setFont(juce::FontOptions(16.f, juce::Font::bold));
    volLbl.setColour(juce::Label::textColourId, tf::dim);
    volume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 40);
    volume.setSliderSnapsToMousePosition(false);
    volAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, "output", volume);
    volume.textFromValueFunction = [](double v) { return juce::String(v, 1) + " dB"; };
    volume.updateText();
    addAndMakeVisible(volLbl);
    addAndMakeVisible(volume);

    browseTitle.setFont(juce::FontOptions(24.f, juce::Font::bold));
    browseTitle.setColour(juce::Label::textColourId, tf::txt);
    closeBtn.onClick = [this] { setMode(main); };
    brandView.setViewedComponent(&brandBox, false);
    brandView.setScrollBarsShown(false, false, false, true);
    list.setRowHeight(60);
    list.getViewport()->setScrollBarsShown(false, false, true, false);
    for (auto* c : { (juce::Component*) &browseTitle, (juce::Component*) &closeBtn, (juce::Component*) &brandView, (juce::Component*) &list })
        addChildComponent(c);

    rebuildChain();
    proc.apvts.state.addListener(this);
    setSize(852, 393);   // iPhone en paysage (points) ; l'appli autonome iOS met la fenêtre en plein écran
    startTimerHz(30);
}

PhoneEditor::~PhoneEditor()
{
    proc.tunerOn = false;   // interface fermée : ne pas laisser le son coupé
    proc.apvts.state.removeListener(this);
    stopTimer();
    setLookAndFeel(nullptr);
}

//==============================================================================
void PhoneEditor::rebuildChain()
{
    shownChain = proc.getChain();
    auto c = shownChain;
    if (!c.contains("pedal"))   // la 1re pédale reste proposée : la choisir l'ajoute à la chaîne
        c.insert(juce::jmax(0, c.indexOf("amp")), "pedal");
    order = { 0 };
    for (auto& nm : c)
        for (int i = 1; i < defs.size(); ++i)
            if (defs[i].unit == nm)   // « amp » : les 4 blocs du cœur
                order.push_back(i);

    chips.clear();
    for (int d : order)
    {
        auto* b = chips.add(new juce::TextButton(defs[d].title));
        b->getProperties().set("pill", true);
        b->onClick = [this, d] { select(d); };
        chipBox.addAndMakeVisible(b);
    }
    if (std::find(order.begin(), order.end(), sel) == order.end())
        select(order.size() > 1 ? *std::find_if(order.begin(), order.end(), [this](int d) { return defs[d].title == "Ampli"; }) : 0);
    else
        refreshChips();
    resized();
}

void PhoneEditor::select(int d)
{
    sel = d;
    const auto& df = defs.getReference(d);
    title.setText(df.title.toUpperCase(), juce::dontSendNotification);
    onAtt.reset();
    onBtn.setVisible(mode == main && df.onId.isNotEmpty());
    if (df.onId.isNotEmpty())
        onAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(proc.apvts, df.onId, onBtn);
    onBtn.onStateChange();

    pickBtns.clear();
    for (int kind : df.kinds)
    {
        const bool ir = kind == 2 || kind == 4;
        auto* b = pickBtns.add(new juce::TextButton(ToneForgeProcessor::pedalSlotOf(kind) >= 0 ? u8("Choisir la pédale…")
                                                    : ir ? (kind == 4 ? u8("Baffle B…") : u8("Choisir le baffle…"))
                                                         : (kind == 3 ? u8("Ampli B…") : u8("Choisir l'ampli…"))));
        b->onClick = [this, kind] { openBrowser(kind); };
        addAndMakeVisible(b);
    }

    params.clear();
    for (int k = 0; k < df.ids.size(); ++k)
    {
        auto pr = std::make_unique<Param>();
        pr->label.setText(df.labels[k], juce::dontSendNotification);
        pr->label.setFont(juce::FontOptions(15.f, juce::Font::bold));
        pr->label.setColour(juce::Label::textColourId, tf::dim);
        pr->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 40);
        pr->slider.setSliderSnapsToMousePosition(false);   // glisser à partir de la position : plus précis au doigt
        pr->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(proc.apvts, df.ids[k], pr->slider);
        if (proc.apvts.getParameterRange(df.ids[k]).end <= 1.f)
            pr->slider.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100)) + " %"; };
        else
            pr->slider.textFromValueFunction = [](double v) { return juce::String(v, 1); };
        pr->slider.updateText();
        paramBox.addAndMakeVisible(pr->label);
        paramBox.addAndMakeVisible(pr->slider);
        params.push_back(std::move(pr));
    }
    paramView.setViewPosition(0, 0);
    refreshName();
    refreshChips();
    resized();
}

bool PhoneEditor::isOn(int d) const
{
    const auto& df = defs.getReference(d);
    for (int kind : df.kinds)   // pédale ou ampli B sans capture : ne joue pas
        if ((ToneForgeProcessor::pedalSlotOf(kind) >= 0 || kind == 3) && proc.loadedFile(kind) == juce::File())
            return false;
    if (df.onId.isNotEmpty())
        return proc.apvts.getRawParameterValue(df.onId)->load() > 0.5f;
    return df.kinds.contains(0) ? proc.loadedFile(0) != juce::File() : true;
}

void PhoneEditor::refreshChips()
{
    for (size_t i = 0; i < order.size(); ++i)
    {
        auto* b = chips[(int) i];
        b->setToggleState(order[i] == sel, juce::dontSendNotification);
        b->setAlpha(isOn(order[i]) ? 1.f : 0.45f);
    }
}

void PhoneEditor::refreshName()
{
    juce::StringArray s;
    for (int kind : defs[sel].kinds)
    {
        const auto f = proc.loadedFile(kind);
        s.add(f == juce::File() ? juce::String("(aucun)") : f.getFileNameWithoutExtension());
    }
    capName.setText(s.joinIntoString("\n"), juce::dontSendNotification);
}

void PhoneEditor::setMode(Mode m)
{
    mode = m;
    proc.tunerOn = m == tune;
    for (auto* c : { (juce::Component*) &chipView, (juce::Component*) &title, (juce::Component*) &capName, (juce::Component*) &paramView,
                     (juce::Component*) &tunerBtn, (juce::Component*) &volLbl, (juce::Component*) &volume })
        c->setVisible(m == main);
    for (auto* b : pickBtns)
        b->setVisible(m == main);
    onBtn.setVisible(m == main && defs[sel].onId.isNotEmpty());
    for (auto* c : { (juce::Component*) &browseTitle, (juce::Component*) &brandView, (juce::Component*) &list })
        c->setVisible(m == browse);
    closeBtn.setVisible(m != main);
    if (m == tune)
    {
        note = "--";
        heard = false;
    }
    resized();
    repaint();
}

//==============================================================================
void PhoneEditor::openBrowser(int kind)
{
    browseKind = kind;
    const bool pedal = ToneForgeProcessor::pedalSlotOf(kind) >= 0, ir = kind == 2 || kind == 4;
    all.clearQuick();
    for (const auto& f : libraryRoot().getChildFile(ir ? "irs" : pedal ? "pedals" : "models")
                             .findChildFiles(juce::File::findFiles, true, ir ? "*.wav" : "*.nam"))
    {
        const auto nm = f.getFileNameWithoutExtension();
        all.add({ f, captureBrand(f.getParentDirectory().getFileName() + " " + nm, pedal), nm });
    }
    std::sort(all.begin(), all.end(), [](const Row& a, const Row& b)   // par marque (« Autres » en dernier), puis par nom
              {
                  const bool ao = a.brand == "Autres", bo = b.brand == "Autres";
                  if (ao != bo)
                      return bo;
                  return a.brand != b.brand ? a.brand.compareNatural(b.brand) < 0 : a.name.compareNatural(b.name) < 0;
              });
    browseTitle.setText((ir ? juce::String("Baffles") : pedal ? u8("Pédales") : juce::String("Amplis")) + (kind == 3 || kind == 4 ? " (B)" : "")
                            + "  ·  " + juce::String(all.size()),
                        juce::dontSendNotification);

    brand.clear();
    brandBtns.clear();
    juce::StringArray names { "Toutes" };
    for (const auto& e : all)
        names.addIfNotAlreadyThere(e.brand);
    for (const auto& n : names)
    {
        auto* b = brandBtns.add(new juce::TextButton(n));
        b->getProperties().set("pill", true);
        b->onClick = [this, n]
        {
            brand = n == "Toutes" ? juce::String() : n;
            showRows();
        };
        brandBox.addAndMakeVisible(b);
    }
    showRows();
    setMode(browse);
    const auto cur = proc.loadedFile(kind);
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i].file == cur && cur != juce::File())
            list.scrollToEnsureRowIsOnscreen(i);
}

void PhoneEditor::showRows()
{
    rows.clearQuick();
    juce::String last;
    for (const auto& e : all)
        if (brand.isEmpty() || e.brand == brand)
        {
            if (e.brand != last)
                rows.add({ {}, e.brand, {} });   // en-tête de marque
            last = e.brand;
            rows.add(e);
        }
    for (auto* b : brandBtns)
        b->setToggleState(b->getButtonText() == (brand.isEmpty() ? juce::String("Toutes") : brand), juce::dontSendNotification);
    list.updateContent();
    list.scrollToEnsureRowIsOnscreen(0);
    list.repaint();
}

void PhoneEditor::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool)
{
    if (!juce::isPositiveAndBelow(row, rows.size()))
        return;
    const auto& e = rows.getReference(row);
    if (e.file == juce::File())
    {
        g.setColour(tf::acc2);
        g.setFont(juce::FontOptions(15.f, juce::Font::bold));
        g.drawText(e.brand.toUpperCase(), 8, h - 28, w - 16, 24, juce::Justification::centredLeft, true);
        return;
    }
    const bool cur = e.file == proc.loadedFile(browseKind);
    const auto r = juce::Rectangle<int>(0, 0, w, h).toFloat().reduced(4.f, 3.f);
    g.setColour(cur ? tf::acc.withAlpha(0.25f) : tf::panel2);
    g.fillRoundedRectangle(r, 10.f);
    g.setColour(cur ? tf::acc : tf::line);
    g.drawRoundedRectangle(r, 10.f, cur ? 2.f : 1.f);
    g.setColour(tf::txt);
    g.setFont(juce::FontOptions(19.f, juce::Font::bold));
    g.drawText(e.name, r.reduced(16.f, 0.f).toNearestInt(), juce::Justification::centredLeft, true);
}

void PhoneEditor::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow(row, rows.size()) && rows[row].file != juce::File())
        load(rows[row].file);
}

void PhoneEditor::load(const juce::File& f)
{
    const int kind = browseKind, ps = ToneForgeProcessor::pedalSlotOf(kind);
    if (f != proc.loadedFile(kind))
    {
        const auto err = kind == 2 || kind == 4 ? proc.loadIR(f, kind == 4) : proc.loadModel(f, ps >= 0, kind == 3, juce::jmax(0, ps));
        if (err.isNotEmpty())
            return juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "ToneForge", err);
    }
    auto turnOn = [this](const juce::String& id)
    {
        if (auto* p = proc.apvts.getParameter(id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(1.f);
            p->endChangeGesture();
        }
    };
    if (ps >= 0)   // choisir une pédale la met dans la chaîne (juste avant l'ampli) et l'allume
    {
        const auto u = ToneForgeProcessor::pedalId(ps);
        if (auto c = proc.getChain(); !c.contains(u))
        {
            c.insert(juce::jmax(0, c.indexOf("amp")), u);
            proc.setChain(c);
        }
        turnOn(u + "_on");
    }
    else if (kind == 3)
        turnOn("ampb_on");
    dirty = true;
    rebuildChain();
    refreshName();
    setMode(main);
}

//==============================================================================
void PhoneEditor::updateTuner()
{
    // Même mesure que l'interface PC : signal moyenné vers ~24 kHz, YIN.
    const double rate = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
    const int dec = juce::jmax(1, juce::roundToInt(rate / 24000.0));
    const double sr = rate / dec;
    proc.readTuner(tunerBuf, juce::jmin(1024 + (int) (sr / 28.0) + 2, 32768 / dec), dec);
    const float f = yinPitch(tunerBuf, sr);
    heard = f > 0.f;
    if (heard)
    {
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const double midi = 69.0 + 12.0 * std::log2(f / 440.0);
        const int n = juce::roundToInt(midi);
        cents = juce::roundToInt((midi - n) * 100.0);
        note = juce::String(names[((n % 12) + 12) % 12]) + juce::String(n / 12 - 1);
        hz = f;
    }
    repaint(tuneArea);
}

void PhoneEditor::timerCallback()
{
    auto fall = [](float& lvl, std::atomic<float>& pk) { lvl = juce::jmax(pk.exchange(0.f), lvl * 0.85f); };
    fall(lvlIn, proc.inPeak);
    fall(lvlOut, proc.outPeak);
    if (mode == main)
        repaint(meterArea.expanded(2));

    if (const int ls = proc.loadSerial.load(); ls != seenLoad)   // capture chargée d'ailleurs (état restauré)
    {
        seenLoad = ls;
        dirty = true;
        refreshName();
        refreshChips();
    }
    if (proc.getChain() != shownChain)   // preset rechargé
        rebuildChain();
    if (++ticks % 15 == 0)
        refreshChips();
    // Toutes les 2 s : réglages sauvegardés s'ils ont changé (iOS peut tuer l'appli sans la fermer)
    if (ticks % 60 == 0)
    {
       #if JucePlugin_Build_Standalone
        if (auto* h = juce::StandalonePluginHolder::getInstance(); h && std::exchange(dirty, false))
            h->savePluginState();
       #endif
    }
    if (mode == tune && ticks % 3 == 0)
        updateTuner();
}

//==============================================================================
void PhoneEditor::paint(juce::Graphics& g)
{
    g.fillAll(tf::bg);
    if (mode == tune)
    {
        auto r = tuneArea;
        const bool inTune = std::abs(cents) <= 3;
        const auto col = (inTune ? tf::ok : std::abs(cents) <= 15 ? tf::warn : tf::bad).withAlpha(heard ? 1.f : 0.4f);
        g.setColour(note == "--" ? tf::dim : col);
        g.setFont(juce::FontOptions(110.f, juce::Font::bold));
        g.drawText(note, r.removeFromTop(r.getHeight() * 11 / 20), juce::Justification::centred);
        // Aiguille : -50 à +50 cents
        auto bar = r.removeFromTop(44).reduced(juce::jmax(20, r.getWidth() / 8), 8).toFloat();
        g.setColour(tf::panel2);
        g.fillRoundedRectangle(bar, 8.f);
        g.setColour(tf::ok.withAlpha(0.35f));
        g.fillRoundedRectangle(bar.withSizeKeepingCentre(bar.getWidth() * 0.06f, bar.getHeight()), 4.f);
        if (note != "--")
        {
            const float x = bar.getX() + bar.getWidth() * (0.5f + juce::jlimit(-50, 50, cents) / 100.f);
            g.setColour(col);
            g.fillRoundedRectangle(juce::Rectangle<float>(10.f, bar.getHeight() + 12.f).withCentre({ x, bar.getCentreY() }), 4.f);
        }
        g.setColour(tf::dim);
        g.setFont(juce::FontOptions(20.f));
        g.drawText(note == "--" ? u8("Joue une corde  ·  son coupé")
                                : (cents > 0 ? "+" : "") + juce::String(cents) + u8(" ¢   ·   ") + juce::String(hz, 1) + " Hz",
                   r, juce::Justification::centred);
        return;
    }
    if (mode != main)
        return;
    // Vumètres entrée / sortie (-60 à 0 dB)
    auto m = meterArea.toFloat();
    for (int i = 0; i < 2; ++i)
    {
        auto row = (i == 0 ? m.removeFromTop(m.getHeight() / 2) : m).reduced(0.f, 3.f);
        g.setColour(tf::dim);
        g.setFont(juce::FontOptions(12.f, juce::Font::bold));
        g.drawText(i == 0 ? "IN" : "OUT", row.removeFromLeft(34.f), juce::Justification::centredLeft);
        g.setColour(tf::panel2);
        g.fillRoundedRectangle(row, 3.f);
        const float db = juce::Decibels::gainToDecibels(i == 0 ? lvlIn : lvlOut, -60.f);
        g.setColour(db > -3.f ? tf::bad : db > -12.f ? tf::warn : tf::ok);
        g.fillRoundedRectangle(row.withWidth(row.getWidth() * (db + 60.f) / 60.f), 3.f);
    }
}

void PhoneEditor::resized()
{
    auto r = getLocalBounds();
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())   // encoche et barre d'accueil
        r = d->safeAreaInsets.subtractedFrom(r);
    r = r.reduced(8);
    auto rowOfPills = [this](juce::Component& box, juce::OwnedArray<juce::TextButton>& btns, int h)
    {
        int x = 0;
        for (auto* b : btns)
        {
            const int w = juce::jmax(96, juce::GlyphArrangement::getStringWidthInt(look.getTextButtonFont(*b, h), b->getButtonText()) + 40);
            b->setBounds(x, 2, w, h);
            x += w + 8;
        }
        box.setSize(x, h + 4);
    };

    if (mode != main)
    {
        auto top = r.removeFromTop(52);
        closeBtn.setBounds(top.removeFromRight(130).reduced(0, 2));
        browseTitle.setBounds(top);
        r.removeFromTop(6);
        if (mode == tune)
        {
            tuneArea = r;
            return;
        }
        brandView.setBounds(r.removeFromTop(52));
        rowOfPills(brandBox, brandBtns, 46);
        r.removeFromTop(6);
        list.setBounds(r);
        return;
    }

    chipView.setBounds(r.removeFromTop(58));
    rowOfPills(chipBox, chips, 52);
    r.removeFromTop(8);
    auto bot = r.removeFromBottom(52);
    tunerBtn.setBounds(bot.removeFromLeft(160));
    bot.removeFromLeft(14);
    auto vol = bot.removeFromRight(juce::jmin(330, bot.getWidth() / 2));
    volLbl.setBounds(vol.removeFromLeft(72));
    volume.setBounds(vol);
    meterArea = bot.reduced(4, 6);
    r.removeFromBottom(8);

    auto left = r.removeFromLeft(juce::jmin(250, r.getWidth() * 2 / 5));
    r.removeFromLeft(14);
    title.setBounds(left.removeFromTop(38));
    if (defs[sel].onId.isNotEmpty())
    {
        onBtn.setBounds(left.removeFromTop(60));
        left.removeFromTop(8);
    }
    for (auto* b : pickBtns)
    {
        b->setBounds(left.removeFromTop(52));
        left.removeFromTop(8);
    }
    capName.setBounds(left);

    paramView.setBounds(r);
    int y = 0;
    for (auto& p : params)
    {
        p->label.setBounds(0, y, r.getWidth(), 22);
        p->slider.setBounds(0, y + 22, r.getWidth(), 44);
        y += 74;
    }
    paramBox.setSize(r.getWidth(), y);
}
