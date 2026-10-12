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
// Habillage « matériel » : textures et pièces dessinées en vectoriel.
namespace
{
using juce::Colour;
namespace Colours = juce::Colours;

// Grain de tolex / cuir : points clairs et sombres semi-transparents, posé en tuile sur une couleur de base.
juce::Image makeGrain()
{
    juce::Image img(juce::Image::ARGB, 128, 128, true);
    juce::Random rnd(1977);
    juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            const bool light = rnd.nextBool();
            const auto v = (juce::uint8) (light ? 255 : 0);
            bd.setPixelColour(x, y, Colour(v, v, v, (juce::uint8) rnd.nextInt(light ? 20 : 46)));
        }
    return img;
}

void textured(juce::Graphics& g, const juce::Path& p, Colour base, const juce::Image& grain)
{
    juce::Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(p);
    g.setColour(base);
    g.fillPath(p);
    g.setTiledImageFill(grain, 0, 0, 1.f);
    g.fillPath(p);
}

void brushed(juce::Graphics& g, juce::Rectangle<float> r, Colour base, int seed)
{
    g.setGradientFill(juce::ColourGradient(base.brighter(0.25f), 0, r.getY(), base.darker(0.3f), 0, r.getBottom(), false));
    g.fillRect(r);
    juce::Random rnd(seed);
    for (int y = (int) r.getY(); y < (int) r.getBottom(); ++y)
    {
        g.setColour((rnd.nextBool() ? Colours::white : Colours::black).withAlpha(rnd.nextFloat() * 0.08f));
        g.drawHorizontalLine(y, r.getX(), r.getRight());
    }
}

void innerShadow(juce::Graphics& g, juce::Rectangle<float> r, float d)
{
    const auto k = Colours::black.withAlpha(0.6f), t = Colours::transparentBlack;
    g.setGradientFill(juce::ColourGradient(k, 0, r.getY(), t, 0, r.getY() + d, false));
    g.fillRect(r.withHeight(d));
    g.setGradientFill(juce::ColourGradient(k, r.getX(), 0, t, r.getX() + d, 0, false));
    g.fillRect(r.withWidth(d));
    g.setGradientFill(juce::ColourGradient(k, r.getRight(), 0, t, r.getRight() - d, 0, false));
    g.fillRect(r.withLeft(r.getRight() - d));
    g.setGradientFill(juce::ColourGradient(k.withAlpha(0.35f), 0, r.getBottom(), t, 0, r.getBottom() - d, false));
    g.fillRect(r.withTop(r.getBottom() - d));
}

// Toile de baffle tissée en diagonale.
void grille(juce::Graphics& g, juce::Rectangle<float> r, Colour base)
{
    juce::Graphics::ScopedSaveState s(g);
    g.reduceClipRegion(r.toNearestInt());
    g.setColour(base);
    g.fillRect(r);
    const float h = r.getHeight();
    for (float d = 0; d < r.getWidth() + h; d += 4.f)
    {
        g.setColour(base.brighter(0.6f).withAlpha(0.3f));
        g.drawLine(r.getX() + d, r.getY(), r.getX() + d - h, r.getBottom(), 1.2f);
        g.setColour(base.darker(0.7f).withAlpha(0.45f));
        g.drawLine(r.getX() + d - h, r.getY(), r.getX() + d, r.getBottom(), 1.2f);
    }
    innerShadow(g, r, 16.f);
}

void screw(juce::Graphics& g, juce::Point<float> c, float rad)
{
    const auto r = juce::Rectangle<float>(rad * 2, rad * 2).withCentre(c);
    g.setGradientFill(juce::ColourGradient(Colour(0xffe8eaed), r.getX(), r.getY(), Colour(0xff5c6168), r.getRight(), r.getBottom(), false));
    g.fillEllipse(r);
    g.setColour(Colours::black.withAlpha(0.7f));
    g.drawEllipse(r, 0.8f);
    g.drawLine({ c.getPointOnCircumference(rad * 0.7f, 0.8f), c.getPointOnCircumference(rad * 0.7f, 0.8f + juce::MathConstants<float>::pi) }, 1.f);
}

void led(juce::Graphics& g, juce::Rectangle<float> r, Colour col, bool on)
{
    if (on)
    {
        g.setColour(col.withAlpha(0.18f));
        g.fillEllipse(r.expanded(6.f));
        g.setColour(col.withAlpha(0.35f));
        g.fillEllipse(r.expanded(2.5f));
    }
    const auto c = on ? col : col.withMultipliedBrightness(0.3f);
    g.setGradientFill(juce::ColourGradient(c.brighter(0.6f), r.getCentreX(), r.getCentreY() - r.getHeight() * 0.2f,
                                           c.darker(0.3f), r.getRight(), r.getBottom(), true));
    g.fillEllipse(r);
    g.setColour(Colours::black.withAlpha(0.7f));
    g.drawEllipse(r, 0.8f);
    g.setColour(Colours::white.withAlpha(on ? 0.7f : 0.25f));
    g.fillEllipse(r.getX() + r.getWidth() * 0.25f, r.getY() + r.getHeight() * 0.18f, r.getWidth() * 0.3f, r.getHeight() * 0.25f);
}

// Couleur du boîtier de chaque pédale, dans l'ordre de la chaîne.
Colour pedalColour(int i)
{
    static const juce::uint32 c[] = { 0xff59606b, 0xff2a6fb8, 0xff262626, 0xff7a4a26,
                                      0xffb9bec5, 0xff7445b0, 0xff2e8b57, 0xff1b8c99 };
    return juce::isPositiveAndBelow(i, (int) std::size(c)) ? Colour(c[i]) : Colour(0xff59606b);
}

// Marque d'une capture d'après son dossier et son nom (modèles connus compris).
juce::String brandOf(const juce::String& s)
{
    static const std::pair<const char*, const char*> rules[] = {
        { "Gallien", "Gallien-Krueger" }, { "GK ", "Gallien-Krueger" }, { "Ampeg", "Ampeg" }, { "SVT", "Ampeg" },
        { "Portaflex", "Ampeg" }, { "B15", "Ampeg" }, { "B-15", "Ampeg" }, { "B18", "Ampeg" }, { "Marshall", "Marshall" },
        { "JCM", "Marshall" }, { "JMP", "Marshall" }, { "Plexi", "Marshall" }, { "Jubilee", "Marshall" }, { "JVM", "Marshall" },
        { "Fender", "Fender" }, { "Bassman", "Fender" }, { "Twin", "Fender" }, { "Deluxe", "Fender" }, { "Princeton", "Fender" },
        { "Mesa", "Mesa/Boogie" }, { "Boogie", "Mesa/Boogie" }, { "Rectifier", "Mesa/Boogie" }, { "Recto", "Mesa/Boogie" },
        { "5150", "EVH / Peavey" }, { "6505", "EVH / Peavey" }, { "EVH", "EVH / Peavey" }, { "Peavey", "EVH / Peavey" },
        { "Orange", "Orange" }, { "Vox", "Vox" }, { "AC30", "Vox" }, { "Friedman", "Friedman" }, { "Soldano", "Soldano" },
        { "Bogner", "Bogner" }, { "Diezel", "Diezel" }, { "ENGL", "ENGL" }, { "Hiwatt", "Hiwatt" }, { "Laney", "Laney" },
        { "Darkglass", "Darkglass" }, { "SansAmp", "Tech 21" }, { "Tech 21", "Tech 21" }, { "Aguilar", "Aguilar" },
        { "Markbass", "Markbass" }, { "Hartke", "Hartke" }, { "Trace Elliot", "Trace Elliot" }, { "Victory", "Victory" },
        { "Revv", "Revv" }, { "Dumble", "Dumble" }, { "Two Rock", "Two-Rock" }, { "Matchless", "Matchless" },
        { "Suhr", "Suhr" }, { "Splawn", "Splawn" }, { "Randall", "Randall" }, { "Blackstar", "Blackstar" },
        { "Celestion", "Celestion" }, { "Greenback", "Celestion" }, { "V30", "Celestion" }, { "Vintage 30", "Celestion" },
        { "Jensen", "Jensen" }, { "Eminence", "Eminence" }, { "OwnHammer", "OwnHammer" }, { "York", "York Audio" },
        { "ML Sound", "ML Sound Lab" }, { "Bergantino", "Bergantino" }, { "Boss", "Boss" }, { "Ibanez", "Ibanez" } };
    for (const auto& [needle, brand] : rules)
        if (s.containsIgnoreCase(needle))
            return brand;
    return "Autres";
}

Colour brandColour(const juce::String& b)
{
    static const std::pair<const char*, juce::uint32> known[] = {
        { "Marshall", 0xffd4af37 }, { "Ampeg", 0xff3f7fe0 }, { "Fender", 0xff8fc4e8 }, { "Mesa/Boogie", 0xffa0a7b0 },
        { "Orange", 0xffff7a1a }, { "Vox", 0xffc0392b }, { "EVH / Peavey", 0xffe74c3c }, { "Celestion", 0xff2ecc71 },
        { "Autres", 0xff8a91a0 } };
    for (const auto& [n, c] : known)
        if (b == n)
            return Colour(c);
    return Colour::fromHSV((float) ((juce::uint32) b.hashCode() % 360u) / 360.f, 0.5f, 0.85f, 1.f);
}
} // namespace

//==============================================================================
TfLook::TfLook()
{
    using namespace tf;
    setColour(juce::ResizableWindow::backgroundColourId, bg);
    setColour(juce::Label::textColourId, txt);
    setColour(juce::Slider::textBoxTextColourId, ink);
    setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::TextEditor::backgroundColourId, bg);
    setColour(juce::TextEditor::outlineColourId, line);
    setColour(juce::TextEditor::focusedOutlineColourId, acc);
    setColour(juce::TextEditor::textColourId, txt);
    setColour(juce::TextButton::buttonColourId, panel2);
    setColour(juce::TextButton::buttonOnColourId, acc);
    setColour(juce::TextButton::textColourOffId, txt);
    setColour(juce::TextButton::textColourOnId, juce::Colour(0xff111111));
    setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff3a3f4a));
    setColour(juce::ToggleButton::textColourId, ink);
}

// Potard chromé : graduations imprimées, arc lumineux de la valeur, jupe moletée, capuchon chromé.
void TfLook::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&)
{
    using juce::Colour;
    namespace Colours = juce::Colours;
    const auto box = juce::Rectangle<int>(x, y, w, h).toFloat();
    const float rad = juce::jmin(box.getWidth(), box.getHeight()) * 0.5f - 15.f;
    const auto c = box.getCentre();
    const float a = a0 + pos * (a1 - a0);

    g.setColour(tf::ink.withAlpha(0.75f));
    for (int i = 0; i <= 10; ++i)
    {
        const float t = a0 + (a1 - a0) * (float) i / 10.f;
        const bool major = i % 5 == 0;
        g.drawLine({ c.getPointOnCircumference(rad + 3.f, t), c.getPointOnCircumference(rad + (major ? 10.f : 7.f), t) },
                   major ? 2.f : 1.2f);
    }
    juce::Path arc;
    arc.addCentredArc(c.x, c.y, rad + 13.f, rad + 13.f, 0.f, a0, a, true);
    g.setColour(tf::acc.withAlpha(0.3f));
    g.strokePath(arc, juce::PathStrokeType(6.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(tf::acc);
    g.strokePath(arc, juce::PathStrokeType(2.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    for (int i = 6; i > 0; --i)
    {
        g.setColour(Colours::black.withAlpha(0.07f));
        g.fillEllipse(juce::Rectangle<float>(rad * 2 + (float) i * 2, rad * 2 + (float) i * 2).withCentre(c.translated(0, 4)));
    }
    const auto skirt = juce::Rectangle<float>(rad * 2, rad * 2).withCentre(c);
    g.setGradientFill(juce::ColourGradient(Colour(0xff3d4045), c.x, skirt.getY(), Colour(0xff0d0e10), c.x, skirt.getBottom(), false));
    g.fillEllipse(skirt);
    for (int i = 0; i < 60; ++i)
    {
        const float t = (float) i * juce::MathConstants<float>::twoPi / 60.f;
        g.setColour(i % 2 ? Colours::white.withAlpha(0.1f) : Colours::black.withAlpha(0.5f));
        g.drawLine({ c.getPointOnCircumference(rad * 0.86f, t), c.getPointOnCircumference(rad - 0.5f, t) }, 1.2f);
    }

    const float cr = rad * 0.66f;
    const auto cap = juce::Rectangle<float>(cr * 2, cr * 2).withCentre(c);
    juce::ColourGradient chrome(Colour(0xfff6f8fa), c.x - cr * 0.4f, c.y - cr * 0.5f, Colour(0xff4c5158), c.x + cr, c.y + cr, true);
    chrome.addColour(0.45, Colour(0xffa9aeb5));
    g.setGradientFill(chrome);
    g.fillEllipse(cap);
    g.setColour(Colours::white.withAlpha(0.07f));
    for (float k = 0.25f; k < 1.f; k += 0.15f)
        g.drawEllipse(juce::Rectangle<float>(cr * 2 * k, cr * 2 * k).withCentre(c), 0.8f);
    g.setColour(Colours::black.withAlpha(0.6f));
    g.drawEllipse(cap, 1.f);

    const auto p0 = c.getPointOnCircumference(rad * 0.3f, a), p1 = c.getPointOnCircumference(rad * 0.97f, a);
    g.setColour(Colours::black.withAlpha(0.6f));
    g.drawLine({ p0, p1 }, 4.5f);
    g.setColour(Colours::white);
    g.drawLine({ p0, p1 }, 2.5f);
}

// Interrupteur à levier chromé + LED.
void TfLook::drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool, bool)
{
    using juce::Colour;
    const bool on = b.getToggleState();
    const juce::Point<float> c(14.f, b.getHeight() * 0.5f);
    const auto tip = c.translated(0.f, on ? -10.f : 10.f);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.drawLine({ c.translated(1.f, 1.f), tip.translated(1.f, 1.f) }, 5.f);
    g.setColour(Colour(0xffd9dce0));
    g.drawLine({ c, tip }, 4.f);
    g.setGradientFill(juce::ColourGradient(Colour(0xfff4f6f8), tip.x - 2, tip.y - 2, Colour(0xff6a6f76), tip.x + 4, tip.y + 4, true));
    g.fillEllipse(juce::Rectangle<float>(8.f, 8.f).withCentre(tip));
    const auto nut = juce::Rectangle<float>(14.f, 14.f).withCentre(c);
    g.setGradientFill(juce::ColourGradient(Colour(0xffe8eaed), nut.getX(), nut.getY(), Colour(0xff50555c), nut.getRight(), nut.getBottom(), false));
    g.fillEllipse(nut);
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.drawEllipse(nut, 1.f);
    led(g, juce::Rectangle<float>(9.f, 9.f).withCentre({ 36.f, c.y }), Colour(0xffff3b30), on);
    g.setColour(tf::ink);
    g.setFont(juce::FontOptions(13.f, juce::Font::bold));
    g.drawText(on ? juce::String("EN MARCHE") : u8("COUPÉ"), b.getLocalBounds().withTrimmedLeft(48), juce::Justification::centredLeft);
}

void TfLook::drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hover, bool down)
{
    using juce::Colour;
    const auto r = b.getLocalBounds().toFloat().reduced(1.f);
    const bool on = b.getToggleState();
    auto top = on ? tf::acc.brighter(0.2f) : Colour(0xff363a43), bot = on ? tf::acc.darker(0.35f) : Colour(0xff1c1f25);
    if (hover)
    {
        top = top.brighter(0.12f);
        bot = bot.brighter(0.12f);
    }
    if (down)
        std::swap(top, bot);
    g.setGradientFill(juce::ColourGradient(top, 0, r.getY(), bot, 0, r.getBottom(), false));
    g.fillRoundedRectangle(r, 6.f);
    g.setColour(juce::Colours::white.withAlpha(on ? 0.35f : 0.09f));
    g.drawHorizontalLine((int) r.getY() + 1, r.getX() + 5.f, r.getRight() - 5.f);
    g.setColour(juce::Colours::black.withAlpha(0.75f));
    g.drawRoundedRectangle(r, 6.f, 1.f);
}

void TfLook::fillTextEditorBackground(juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    const auto r = juce::Rectangle<int>(0, 0, w, h).toFloat();
    g.setColour(juce::Colour(0xff0a0b0e));
    g.fillRoundedRectangle(r, 6.f);
    g.setGradientFill(juce::ColourGradient(juce::Colours::black.withAlpha(0.7f), 0, 0, juce::Colours::transparentBlack, 0, 7.f, false));
    g.fillRoundedRectangle(r.withHeight(8.f), 6.f);
}

void TfLook::drawTextEditorOutline(juce::Graphics& g, int w, int h, juce::TextEditor& te)
{
    const auto r = juce::Rectangle<int>(0, 0, w, h).toFloat().reduced(0.5f);
    g.setColour(te.hasKeyboardFocus(true) ? tf::acc : juce::Colours::black.withAlpha(0.8f));
    g.drawRoundedRectangle(r, 6.f, 1.f);
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawHorizontalLine(h - 1, 6.f, (float) w - 6.f);
}

//==============================================================================
ToneForgeEditor::ToneForgeEditor(ToneForgeProcessor& p) : AudioProcessorEditor(&p), proc(p)
{
    setLookAndFeel(&look);
    grain = makeGrain();

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
    defs.add({ "EQ", u8("TONALITÉ"), {}, "Graves 120 Hz, Médiums 800 Hz, Aigus 3,5 kHz", { "bass", "mid", "treble" },
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
    list.setRowHeight(46);
    addAndMakeVisible(list);

    // Écran LCD, plaque et grille
    nameLbl.setFont(juce::FontOptions("Consolas", 19.f, juce::Font::bold));
    nameLbl.setColour(juce::Label::textColourId, tf::amber);
    nameLbl.setJustificationType(juce::Justification::centred);
    warnLbl.setColour(juce::Label::textColourId, tf::amber.withAlpha(0.75f));
    warnLbl.setFont(juce::FontOptions("Consolas", 11.f, juce::Font::plain));
    warnLbl.setJustificationType(juce::Justification::centred);
    panelTitle.setFont(juce::FontOptions(16.f, juce::Font::bold));
    panelTitle.setColour(juce::Label::textColourId, tf::ink);
    panelInfo.setColour(juce::Label::textColourId, tf::cream.withAlpha(0.75f));
    panelInfo.setJustificationType(juce::Justification::centred);
    for (auto* l : { &nameLbl, &warnLbl, &panelTitle, &panelInfo })
        addAndMakeVisible(l);
    bypass.setButtonText("Actif");
    addChildComponent(bypass);

    scanLibrary();
    proc.apvts.state.addListener(this);
    selectBlock(selected);
    refreshLabels();
    setSize(1200, 720);
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
        {
            const auto grp = f.getParentDirectory().getFileName(), nm = f.getFileNameWithoutExtension();
            out.add({ f, grp, nm, brandOf(grp + " " + nm) });
        }
        // Par marque (« Autres » en dernier), puis par dossier et nom
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b)
                  {
                      const bool ao = a.brand == "Autres", bo = b.brand == "Autres";
                      if (ao != bo)
                          return bo;
                      if (a.brand != b.brand)
                          return a.brand.compareNatural(b.brand) < 0;
                      return (a.group + a.name).compareNatural(b.group + b.name) < 0;
                  });
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
    int count = 0;
    juce::String last;
    for (const auto& e : showModels ? models : irs)
        if (q.isEmpty() || (e.group + " " + e.name + " " + e.brand).containsIgnoreCase(q))
        {
            if (e.brand != last)
                shown.add({ {}, {}, e.brand, e.brand });   // en-tête de marque
            last = e.brand;
            shown.add(e);
            ++count;
        }
    ampTab.setToggleState(showModels, juce::dontSendNotification);
    cabTab.setToggleState(!showModels, juce::dontSendNotification);
    ampTab.setButtonText("Amplis (" + juce::String(models.size()) + ")");
    cabTab.setButtonText("Baffles (" + juce::String(irs.size()) + ")");
    countLbl.setText(juce::String(count) + (showModels ? " captures" : " IR"), juce::dontSendNotification);
    list.updateContent();
    list.repaint();
}

void ToneForgeEditor::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool)
{
    if (!juce::isPositiveAndBelow(row, shown.size()))
        return;
    const auto& e = shown.getReference(row);
    const auto bc = brandColour(e.brand);
    if (e.file == juce::File())
    {
        const auto t = juce::Rectangle<int>(6, h - 24, w - 12, 20);
        g.setColour(bc);
        g.setFont(juce::FontOptions(12.f, juce::Font::bold));
        g.drawText(e.name.toUpperCase(), t, juce::Justification::centredLeft, true);
        g.setColour(bc.withAlpha(0.35f));
        g.fillRect(t.getX(), t.getBottom() - 1, t.getWidth(), 1);
        return;
    }
    const bool current = e.file == (showModels ? proc.modelFile : proc.irFile);
    const auto r = juce::Rectangle<int>(0, 0, w, h).toFloat().reduced(3.f, 2.f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff272b32), 0, r.getY(), juce::Colour(0xff1b1e23), 0, r.getBottom(), false));
    g.fillRoundedRectangle(r, 7.f);
    if (current)
    {
        g.setColour(tf::acc.withAlpha(0.12f));
        g.fillRoundedRectangle(r, 7.f);
    }
    g.setColour(current ? tf::acc : juce::Colours::black.withAlpha(0.6f));
    g.drawRoundedRectangle(r, 7.f, current ? 1.6f : 1.f);
    g.setColour(bc);
    g.fillRoundedRectangle(r.withWidth(4.f), 2.f);

    const auto badge = juce::Rectangle<float>(28.f, 28.f).withCentre({ r.getX() + 24.f, r.getCentreY() });
    g.setGradientFill(juce::ColourGradient(bc.brighter(0.3f), badge.getX(), badge.getY(), bc.darker(0.45f), badge.getRight(), badge.getBottom(), false));
    g.fillEllipse(badge);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawEllipse(badge, 1.f);
    g.setColour(bc.getPerceivedBrightness() > 0.6f ? tf::ink : juce::Colours::white);
    g.setFont(juce::FontOptions(11.f, juce::Font::bold));
    g.drawText(e.brand.substring(0, 2).toUpperCase(), badge.toNearestInt(), juce::Justification::centred);

    auto t = r.withTrimmedLeft(44.f).reduced(4.f, 4.f).toNearestInt();
    if (current)
        led(g, juce::Rectangle<float>(8.f, 8.f).withCentre({ (float) t.getRight() - 4.f, r.getCentreY() }), tf::amber, true);
    if (current)
        t.removeFromRight(16);
    g.setColour(tf::txt);
    g.setFont(juce::FontOptions(13.f, juce::Font::bold));
    g.drawText(e.name, t.removeFromTop(19), juce::Justification::centredLeft, true);
    g.setColour(tf::dim);
    g.setFont(11.f);
    g.drawText(e.group, t, juce::Justification::centredLeft, true);
}

void ToneForgeEditor::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow(row, shown.size()) && shown[row].file != juce::File())
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
    panelTitle.setText(d.title.toUpperCase(), juce::dontSendNotification);
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
        kn->label.setColour(juce::Label::textColourId, tf::ink);
        kn->label.setFont(juce::FontOptions(12.f, juce::Font::bold));
        kn->slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        kn->slider.setColour(juce::Slider::textBoxTextColourId, tf::ink);
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

// Pédale : boîtier coloré, LED rouge, étiquette, interrupteur au pied chromé.
void ToneForgeEditor::Block::paint(juce::Graphics& g)
{
    using juce::Colour;
    namespace Colours = juce::Colours;
    const auto& d = ed.defs.getReference(index);
    const bool on = ed.blockOn(index), sel = ed.selected == index;
    const auto col = pedalColour(index);
    const auto r = getLocalBounds().toFloat().reduced(5.f, 4.f);

    if (sel)
        for (int i = 1; i <= 3; ++i)
        {
            g.setColour(tf::acc.withAlpha(0.14f * (float) (4 - i)));
            g.drawRoundedRectangle(r.expanded((float) i), 9.f + (float) i, 2.f);
        }
    g.setColour(Colours::black.withAlpha(0.5f));
    g.fillRoundedRectangle(r.translated(0.f, 3.f), 9.f);
    juce::Path body;
    body.addRoundedRectangle(r, 9.f);
    g.setGradientFill(juce::ColourGradient(col.brighter(0.25f), 0, r.getY(), col.darker(0.45f), 0, r.getBottom(), false));
    g.fillPath(body);
    if (index == 2)   // ampli : tolex noir à liseré or
    {
        textured(g, body, col, ed.grain);
        g.setColour(Colour(0xffc9a65a));
        g.drawRoundedRectangle(r.reduced(3.f), 7.f, 1.2f);
    }
    if (!on)
    {
        g.setColour(Colours::black.withAlpha(0.38f));
        g.fillPath(body);
    }
    g.setColour(Colours::white.withAlpha(0.22f));
    g.drawHorizontalLine((int) r.getY() + 1, r.getX() + 8.f, r.getRight() - 8.f);
    g.setColour(Colours::black.withAlpha(0.85f));
    g.strokePath(body, juce::PathStrokeType(1.f));
    for (auto pt : { r.getTopLeft().translated(7.f, 7.f), r.getTopRight().translated(-7.f, 7.f),
                     r.getBottomLeft().translated(7.f, -7.f), r.getBottomRight().translated(-7.f, -7.f) })
        screw(g, pt, 2.3f);

    const auto ink = (col.getPerceivedBrightness() > 0.6f ? tf::ink : Colours::white).withAlpha(on ? 1.f : 0.55f);
    led(g, juce::Rectangle<float>(9.f, 9.f).withCentre({ r.getCentreX(), r.getY() + 12.f }), Colour(0xffff3b30), on);
    g.setColour(ink.withMultipliedAlpha(0.65f));
    g.setFont(juce::FontOptions(9.f, juce::Font::bold));
    g.drawText(d.kind.toUpperCase(), r.withTrimmedTop(21.f).withHeight(12.f).toNearestInt(), juce::Justification::centred, true);
    g.setColour(ink);
    g.setFont(juce::FontOptions(15.f, juce::Font::bold));
    g.drawText(d.title, r.withTrimmedTop(33.f).withHeight(22.f).toNearestInt(), juce::Justification::centred, true);

    const juce::Point<float> fc(r.getCentreX(), r.getBottom() - 22.f);
    g.setColour(Colour(0xff151515));
    g.fillEllipse(juce::Rectangle<float>(28.f, 28.f).withCentre(fc));
    juce::ColourGradient chrome(Colour(0xfff6f8fa), fc.x - 4, fc.y - 5, Colour(0xff50555c), fc.x + 11, fc.y + 11, true);
    chrome.addColour(0.5, Colour(0xffa9aeb5));
    g.setGradientFill(chrome);
    g.fillEllipse(juce::Rectangle<float>(22.f, 22.f).withCentre(fc));
    g.setGradientFill(juce::ColourGradient(Colour(0xff7a7f86), fc.x, fc.y - 6, Colour(0xffe6e8eb), fc.x, fc.y + 6, false));
    g.fillEllipse(juce::Rectangle<float>(12.f, 12.f).withCentre(fc));
    g.setColour(Colours::black.withAlpha(0.6f));
    g.drawEllipse(juce::Rectangle<float>(22.f, 22.f).withCentre(fc), 1.f);
}

void ToneForgeEditor::Block::mouseUp(const juce::MouseEvent& e)
{
    // Interrupteur au pied ou LED = marche/arrêt ; ailleurs = afficher les réglages du bloc.
    const auto r = getLocalBounds().toFloat().reduced(5.f, 4.f);
    const bool onSwitch = e.position.getDistanceFrom({ r.getCentreX(), r.getBottom() - 22.f }) < 16.f
                       || e.position.getDistanceFrom({ r.getCentreX(), r.getY() + 12.f }) < 12.f;
    if (ed.defs[index].onId.isNotEmpty() && onSwitch)
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
    // Aiguilles avec inertie, comme un vrai VU
    needleIn += (juce::Decibels::gainToDecibels(levelIn, -60.f) - needleIn) * 0.3f;
    needleOut += (juce::Decibels::gainToDecibels(levelOut, -60.f) - needleOut) * 0.3f;
    repaint(meterIn.getUnion(meterOut).expanded(2));

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

// Décor statique pré-rendu : tolex, barre en métal brossé, écran LCD, câbles, façade d'ampli.
void ToneForgeEditor::renderBackground()
{
    using juce::Colour;
    namespace Colours = juce::Colours;
    if (getWidth() <= 0 || getHeight() <= 0)
        return;
    bgCache = juce::Image(juce::Image::RGB, getWidth(), getHeight(), true);
    juce::Graphics g(bgCache);
    const float W = (float) getWidth();

    juce::Path all;
    all.addRectangle(getLocalBounds().toFloat());
    textured(g, all, Colour(0xff121316), grain);

    // Barre du haut
    brushed(g, topBar.toFloat(), Colour(0xff2b2e34), 7);
    g.setColour(Colours::black);
    g.drawHorizontalLine(topBar.getBottom() - 1, 0.f, W);
    for (auto pt : { juce::Point<float>(9.f, 9.f), { 9.f, (float) topBar.getBottom() - 10.f },
                     { W - 9.f, 9.f }, { W - 9.f, (float) topBar.getBottom() - 10.f } })
        screw(g, pt, 3.f);
    const juce::Rectangle<int> logo(24, 6, 240, 38);
    g.setFont(juce::FontOptions("Segoe UI", 30.f, juce::Font::bold | juce::Font::italic));
    g.setColour(tf::acc.withAlpha(0.18f));
    for (auto o : { juce::Point<int>(-2, 0), { 2, 0 }, { 0, -2 }, { 0, 2 } })
        g.drawText("ToneForge", logo + o, juce::Justification::centredLeft);
    g.setColour(Colours::black.withAlpha(0.7f));
    g.drawText("ToneForge", logo + juce::Point<int>(1, 2), juce::Justification::centredLeft);
    g.setGradientFill(juce::ColourGradient(tf::acc.brighter(0.5f), 0, (float) logo.getY() + 6.f, tf::acc.darker(0.3f), 0, (float) logo.getBottom() - 6.f, false));
    g.drawText("ToneForge", logo, juce::Justification::centredLeft);
    g.setColour(tf::dim);
    g.setFont(juce::FontOptions(9.f, juce::Font::bold));
    g.drawText(u8("NEURAL AMP  ·  AIRWINDOWS"), 26, 42, 240, 12, juce::Justification::centredLeft);

    // Écran LCD ambre
    const auto lcd = lcdArea.toFloat();
    g.setColour(Colours::black);
    g.fillRoundedRectangle(lcd.expanded(3.f), 7.f);
    g.setGradientFill(juce::ColourGradient(Colour(0xff1c1307), 0, lcd.getY(), Colour(0xff0b0803), 0, lcd.getBottom(), false));
    g.fillRoundedRectangle(lcd, 5.f);
    g.setColour(Colours::black.withAlpha(0.22f));
    for (float y = lcd.getY(); y < lcd.getBottom(); y += 2.f)
        g.drawHorizontalLine((int) y, lcd.getX(), lcd.getRight());
    innerShadow(g, lcd, 6.f);
    g.setColour(Colours::white.withAlpha(0.04f));
    g.fillRoundedRectangle(lcd.withHeight(lcd.getHeight() * 0.45f), 5.f);

    // Panneau latéral en cuir
    juce::Path side;
    side.addRectangle(0.f, (float) topBar.getBottom(), 260.f, (float) getHeight() - (float) topBar.getBottom());
    textured(g, side, Colour(0xff17191d), grain);
    g.setColour(Colours::black);
    g.drawVerticalLine(260, (float) topBar.getBottom(), (float) getHeight());
    g.setColour(Colours::white.withAlpha(0.05f));
    g.drawVerticalLine(261, (float) topBar.getBottom(), (float) getHeight());

    // Câbles jack entre les pédales
    for (size_t i = 0; i + 1 < blocks.size(); ++i)
    {
        const auto a = blocks[i]->getBounds().toFloat(), b = blocks[i + 1]->getBounds().toFloat();
        const float y = a.getY() + a.getHeight() * 0.42f;
        const juce::Point<float> p0(a.getRight() - 4.f, y), p1(b.getX() + 4.f, y);
        juce::Path cable;
        cable.startNewSubPath(p0);
        cable.cubicTo(p0.translated(8.f, 22.f), p1.translated(-8.f, 22.f), p1);
        g.setColour(Colour(0xff0a0a0a));
        g.strokePath(cable, juce::PathStrokeType(4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(Colours::white.withAlpha(0.12f));
        g.strokePath(cable, juce::PathStrokeType(1.f), juce::AffineTransform::translation(0.f, -1.f));
        for (auto pt : { p0, p1 })
        {
            const auto j = juce::Rectangle<float>(7.f, 9.f).withCentre(pt);
            g.setGradientFill(juce::ColourGradient(Colour(0xffe8eaed), j.getX(), j.getY(), Colour(0xff5c6168), j.getX(), j.getBottom(), false));
            g.fillRoundedRectangle(j, 1.5f);
        }
    }

    // Façade d'ampli : tolex, liseré crème, plaque de commande, grille
    const auto pa = panelArea.toFloat();
    juce::DropShadow(Colours::black.withAlpha(0.7f), 18, { 0, 6 }).drawForRectangle(g, panelArea);
    juce::Path amp;
    amp.addRoundedRectangle(pa, 12.f);
    textured(g, amp, Colour(0xff151515), grain);
    g.setColour(Colours::black);
    g.strokePath(amp, juce::PathStrokeType(1.5f));
    g.setColour(tf::cream.withAlpha(0.85f));
    g.drawRoundedRectangle(pa.reduced(7.f), 8.f, 2.5f);

    const auto pl = plateArea.toFloat();
    const auto plateCol = selected == 2 ? Colour(0xffc9a65a) : pedalColour(selected).interpolatedWith(Colour(0xffd3d6db), 0.62f);
    juce::DropShadow(Colours::black.withAlpha(0.6f), 8, { 0, 3 }).drawForRectangle(g, plateArea);
    {
        juce::Graphics::ScopedSaveState s(g);
        juce::Path pp;
        pp.addRoundedRectangle(pl, 6.f);
        g.reduceClipRegion(pp);
        brushed(g, pl, plateCol, 11 + selected);
    }
    g.setColour(Colours::white.withAlpha(0.35f));
    g.drawHorizontalLine((int) pl.getY() + 1, pl.getX() + 6.f, pl.getRight() - 6.f);
    g.setColour(Colours::black.withAlpha(0.75f));
    g.drawRoundedRectangle(pl, 6.f, 1.f);
    for (auto pt : { pl.getTopLeft().translated(10.f, 10.f), pl.getTopRight().translated(-10.f, 10.f),
                     pl.getBottomLeft().translated(10.f, -10.f), pl.getBottomRight().translated(-10.f, -10.f) })
        screw(g, pt, 3.5f);

    const auto gr = grilleArea.toFloat();
    grille(g, gr, selected == 3 ? Colour(0xff3a2c1c) : Colour(0xff2a2620));
    g.setColour(Colours::black);
    g.drawRect(gr, 1.5f);
    const auto logoArea = gr.withTrimmedBottom(40.f).toNearestInt();
    g.setFont(juce::FontOptions("Segoe Script", 56.f, juce::Font::bold));
    g.setColour(Colours::black.withAlpha(0.75f));
    g.drawText(defs[selected].title, logoArea + juce::Point<int>(3, 4), juce::Justification::centred);
    g.setGradientFill(juce::ColourGradient(Colour(0xfffff7e0), 0, (float) logoArea.getCentreY() - 24.f, Colour(0xffc9b88a), 0, (float) logoArea.getCentreY() + 24.f, false));
    g.drawText(defs[selected].title, logoArea, juce::Justification::centred);
}

void ToneForgeEditor::paint(juce::Graphics& g)
{
    if (bgCache.isValid())
        g.drawImageAt(bgCache, 0, 0);
    else
        g.fillAll(tf::bg);

    // VU-mètres à aiguille (-30 à +3 dB, zone rouge au-dessus de 0)
    auto vu = [&g](juce::Rectangle<int> box, float db, const char* lbl)
    {
        using juce::Colour;
        const auto r = box.toFloat();
        g.setColour(juce::Colours::black);
        g.fillRoundedRectangle(r, 4.f);
        const auto face = r.reduced(3.f);
        g.setGradientFill(juce::ColourGradient(Colour(0xfff6ecd0), 0, face.getY(), Colour(0xffd6c394), 0, face.getBottom(), false));
        g.fillRoundedRectangle(face, 3.f);
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(face.toNearestInt());
        const juce::Point<float> piv(face.getCentreX(), face.getBottom() + face.getHeight() * 0.35f);
        const float R = face.getHeight() * 1.08f;
        auto ang = [](float d) { return -0.8f + 1.6f * juce::jlimit(0.f, 1.f, (d + 30.f) / 33.f); };
        juce::Path scale, red;
        scale.addCentredArc(piv.x, piv.y, R, R, 0.f, ang(-30.f), ang(0.f), true);
        red.addCentredArc(piv.x, piv.y, R, R, 0.f, ang(0.f), ang(3.f), true);
        g.setColour(tf::ink);
        g.strokePath(scale, juce::PathStrokeType(1.2f));
        g.setColour(juce::Colour(0xffd0302a));
        g.strokePath(red, juce::PathStrokeType(3.f));
        for (float d : { -20.f, -10.f, -7.f, -5.f, -3.f, -1.f, 0.f, 1.f, 2.f, 3.f })
        {
            g.setColour(d > 0.f ? juce::Colour(0xffd0302a) : tf::ink);
            g.drawLine({ piv.getPointOnCircumference(R, ang(d)), piv.getPointOnCircumference(R + 5.f, ang(d)) }, 1.f);
        }
        g.setColour(tf::ink);
        g.setFont(juce::FontOptions(9.f, juce::Font::bold));
        g.drawText("VU", face.withTrimmedTop(face.getHeight() * 0.45f).toNearestInt(), juce::Justification::centred);
        g.setFont(8.f);
        g.drawText(lbl, face.reduced(4.f, 2.f).toNearestInt(), juce::Justification::bottomLeft);
        g.setColour(juce::Colour(0xff111111));
        g.drawLine({ piv, piv.getPointOnCircumference(R + 3.f, ang(db)) }, 1.4f);
        g.fillRect(face.withTop(face.getBottom() - 5.f));
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillRect(face.withHeight(face.getHeight() * 0.4f));
    };
    vu(meterIn, needleIn, "IN");
    vu(meterOut, needleOut, "OUT");
}

void ToneForgeEditor::resized()
{
    auto r = getLocalBounds();
    topBar = r.removeFromTop(64);
    {
        auto t = topBar.reduced(20, 8);
        meterOut = t.removeFromRight(86);
        t.removeFromRight(8);
        meterIn = t.removeFromRight(86);
        lcdArea = topBar.withSizeKeepingCentre(470, 46);
        auto l = lcdArea.reduced(12, 3);
        nameLbl.setBounds(l.removeFromTop(25));
        warnLbl.setBounds(l);
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
    chainArea = c.removeFromTop(118);
    {
        const int n = (int) blocks.size(), gap = 16;
        const int bw = (chainArea.getWidth() - gap * (n - 1)) / n;
        auto row = chainArea;
        for (auto& b : blocks)
        {
            b->setBounds(row.removeFromLeft(bw));
            row.removeFromLeft(gap);
        }
    }
    c.removeFromTop(14);
    panelArea = c;
    auto p = panelArea.reduced(20);
    plateArea = p.removeFromTop(212);
    p.removeFromTop(14);
    grilleArea = p;

    auto q = plateArea.reduced(24, 12);
    auto head = q.removeFromTop(30);
    panelTitle.setBounds(head.removeFromLeft(300));
    bypass.setBounds(head.removeFromRight(150));
    q.removeFromTop(4);
    const int kw = 110, kg = 18, n = (int) knobs.size();
    q = q.withSizeKeepingCentre(juce::jmax(0, n * kw + (n - 1) * kg), q.getHeight());
    for (auto& k : knobs)
    {
        auto col = q.removeFromLeft(kw);
        q.removeFromLeft(kg);
        k->label.setBounds(col.removeFromTop(18));
        k->slider.setBounds(col);
    }
    panelInfo.setBounds(grilleArea.reduced(20, 10).removeFromBottom(24));

    renderBackground();
}
