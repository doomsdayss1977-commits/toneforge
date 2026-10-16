#include "PluginEditor.h"
#include <regex>
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
// Majuscules sans dépendre de la locale (towupper laisse « é » en minuscule) : Latin-1 compris.
static juce::String upper(const juce::String& s)
{
    juce::String r;
    for (auto c : s)
        r += (c >= 0xe0 && c <= 0xfe && c != 0xf7) ? (juce::juce_wchar) (c - 0x20) : juce::CharacterFunctions::toUpperCase(c);
    return r;
}

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
    static const juce::uint32 c[] = { 0xff59606b, 0xff2a6fb8, 0xffb8322a, 0xff262626, 0xff7a4a26, 0xff4a3524,
                                      0xffb9bec5, 0xff7445b0, 0xff2e8b57, 0xff1b8c99 };
    return juce::isPositiveAndBelow(i, (int) std::size(c)) ? Colour(c[i]) : Colour(0xff59606b);
}

// Marque d'une capture d'après son dossier et son nom (modèles connus compris).
// Pédales : règles à part, consultées en premier et seulement pour les pédales
// (sinon « 5150 + TS9 » ou « into Klon » classeraient un ampli chez un fabricant de pédales).
juce::String brandOf(const juce::String& s, bool pedal = false)
{
    // Behringer avant Klon : « Behringer Centaur Overdrive KLON »
    static const std::pair<const char*, const char*> pedalRules[] = {
        { "Tech21", "Tech 21" }, { "B7K", "Darkglass" }, { "Behringer", "Behringer" }, { "TS9", "Ibanez" },
        { "TS808", "Ibanez" }, { "Tube Screamer", "Ibanez" }, { "Klon", "Klon" }, { "Centaur", "Klon" },
        { "EHX", "Electro-Harmonix" }, { "Electro-Harmonix", "Electro-Harmonix" }, { "Big Muff", "Electro-Harmonix" },
        { "ProCo", "ProCo" }, { "Pro Co", "ProCo" }, { "RAT", "ProCo" }, { "Wampler", "Wampler" },
        { "Death By Audio", "Death By Audio" }, { "Fuzz War", "Death By Audio" },
        { "Mosrite", "Mosrite" }, { "Fuzzrite", "Mosrite" } };
    static const std::pair<const char*, const char*> rules[] = {
        { "Gallien", "Gallien-Krueger" }, { "GK", "Gallien-Krueger" }, { "Ampeg", "Ampeg" }, { "SVT", "Ampeg" },
        { "Portaflex", "Ampeg" }, { "B15", "Ampeg" }, { "B-15", "Ampeg" }, { "B18", "Ampeg" }, { "Marshall", "Marshall" },
        { "JCM", "Marshall" }, { "JMP", "Marshall" }, { "Plexi", "Marshall" }, { "Jubilee", "Marshall" }, { "JVM", "Marshall" },
        { "Fender", "Fender" }, { "Bassman", "Fender" }, { "Twin", "Fender" }, { "Deluxe", "Fender" }, { "Princeton", "Fender" },
        { "Mesa", "Mesa/Boogie" }, { "Boogie", "Mesa/Boogie" }, { "Rectifier", "Mesa/Boogie" }, { "Recto", "Mesa/Boogie" },
        { "5150", "EVH / Peavey" }, { "6505", "EVH / Peavey" }, { "EVH", "EVH / Peavey" }, { "Peavey", "EVH / Peavey" },
        { "Orange", "Orange" }, { "Vox", "Vox" }, { "AC30", "Vox" }, { "Friedman", "Friedman" }, { "Soldano", "Soldano" },
        { "Bogner", "Bogner" }, { "Diezel", "Diezel" }, { "ENGL", "ENGL" }, { "Hiwatt", "Hiwatt" }, { "Laney", "Laney" },
        { "Darkglass", "Darkglass" }, { "SansAmp", "Tech 21" }, { "Tech 21", "Tech 21" }, { "Aguilar", "Aguilar" },
        { "Markbass", "Markbass" }, { "Hartke", "Hartke" }, { "Trace Elliot", "Trace Elliot" }, { "Victory", "Victory" },
        { "Revv", "Revv" }, { "Dumble", "Dumble" }, { "Two Rock", "Two-Rock" }, { "Two-Rock", "Two-Rock" }, { "Matchless", "Matchless" },
        { "Suhr", "Suhr" }, { "Splawn", "Splawn" }, { "Randall", "Randall" }, { "Blackstar", "Blackstar" },
        { "Celestion", "Celestion" }, { "Greenback", "Celestion" }, { "V30", "Celestion" }, { "Vintage 30", "Celestion" },
        { "Jensen", "Jensen" }, { "Eminence", "Eminence" }, { "OwnHammer", "OwnHammer" }, { "York Audio", "York Audio" },
        { "ML Sound", "ML Sound Lab" }, { "Bergantino", "Bergantino" }, { "Boss", "Boss" }, { "Ibanez", "Ibanez" } };
    // Mot entier : pas de lettre juste avant, ni juste après un mot-clé finissant par une lettre
    // (JCM800, B15N acceptés ; Twins, Bossa refusés).
    auto hasWord = [&s](const juce::String& w)
    {
        for (int i = s.indexOfIgnoreCase(w); i >= 0; i = s.indexOfIgnoreCase(i + 1, w))
        {
            const auto end = i + w.length();
            if ((i == 0 || !juce::CharacterFunctions::isLetter(s[i - 1]))
                && (end >= s.length() || !juce::CharacterFunctions::isLetter(w.getLastCharacter())
                    || !juce::CharacterFunctions::isLetter(s[end])))
                return true;
        }
        return false;
    };
    if (pedal)
        for (const auto& [needle, brand] : pedalRules)
            if (hasWord(needle))
                return brand;
    for (const auto& [needle, brand] : rules)
        if (hasWord(needle))
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

// Habillage de la façade selon la marque de la capture chargée.
struct Skin
{
    Colour tolex, piping, plate, grille, logo;
    const char* font;
    float size;
    bool italic;
};
Skin skinFor(const juce::String& b)
{
    if (b == "Marshall")    return { Colour(0xff141414), Colour(0xfff2f2f2), Colour(0xffc9a65a), Colour(0xff2b2722), Colour(0xfff4f1e8), "Segoe Script", 60.f, false };
    if (b == "Fender")      return { Colour(0xff111111), Colour(0xffc8ccd2), Colour(0xff1b1c1f), Colour(0xff8f9399), Colour(0xffe9ecef), "Segoe Script", 60.f, false };
    if (b == "Ampeg")       return { Colour(0xff121212), Colour(0xffc8ccd2), Colour(0xffb9c0c9), Colour(0xff1c1d20), Colour(0xff3f7fe0), "Segoe Script", 60.f, false };
    if (b == "Mesa/Boogie") return { Colour(0xff0c0c0c), Colour(0xff4a4d52), Colour(0xff17181a), Colour(0xff151515), Colour(0xffc8ccd2), "Georgia", 48.f, true };
    if (b == "Orange")      return { Colour(0xffe8701a), Colour(0xff1a1a1a), Colour(0xffefe6d2), Colour(0xffcfc2a0), Colour(0xff151515), "Arial Black", 54.f, false };
    if (b == "Vox")         return { Colour(0xff141414), Colour(0xffc9a65a), Colour(0xffa0522d), Colour(0xff4a3020), Colour(0xfff4f1e8), "Arial Black", 58.f, false };
    if (b == "EVH / Peavey")return { Colour(0xff0f0f0f), Colour(0xff6a6d72), Colour(0xff1a1a1a), Colour(0xff181818), Colour(0xffe74c3c), "Impact", 58.f, false };
    if (b == "Gallien-Krueger") return { Colour(0xff151515), Colour(0xff6a6d72), Colour(0xff2a2b2e), Colour(0xff1d1d1d), Colour(0xffe9ecef), "Impact", 52.f, false };
    return { Colour(0xff151515), Colour(0xfff1e6c8), Colour(0xffc9a65a), Colour(0xff2a2620), Colour(0xfff4f1e8), "Segoe Script", 56.f, false };
}
} // namespace

// TapeDelay2 : la bande (88 200 cases) avance de A^4*25+1 cases par cycle, un cycle = floor(sr/44100) échantillons
// (1 à 4) ; l'écho revient après un tour complet. Le flutter par défaut (0,1) y change moins de 0,01 %.
static int tapeCycle(double sr) { return juce::jlimit(1, 4, (int) std::floor(sr / 44100.0)); }
static double delayMs(double a, double sr)
{
    sr = sr > 0 ? sr : 48000.0;
    return 88200.0 * tapeCycle(sr) * 1000.0 / (sr * (std::pow(a, 4.0) * 25.0 + 1.0));
}
static double delayNorm(double ms, double sr)
{
    sr = sr > 0 ? sr : 48000.0;
    const double speed = 88200.0 * tapeCycle(sr) * 1000.0 / (sr * juce::jmax(1.0, ms));
    return std::pow(juce::jlimit(0.0, 1.0, (speed - 1.0) / 25.0), 0.25);
}

juce::File libraryRoot() { return kRoot; }
juce::String captureBrand(const juce::String& s, bool pedal) { return brandOf(s, pedal); }

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
void TfLook::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& sl)
{
    using juce::Colour;
    namespace Colours = juce::Colours;
    const auto box = juce::Rectangle<int>(x, y, w, h).toFloat();
    const float rad = juce::jmin(box.getWidth(), box.getHeight()) * 0.5f - 15.f;
    const auto c = box.getCentre();
    const float a = a0 + pos * (a1 - a0);

    g.setColour(sl.findColour(juce::Slider::textBoxTextColourId).withAlpha(0.75f));
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
    g.setColour(b.findColour(juce::ToggleButton::textColourId));
    g.setFont(juce::FontOptions(13.f, juce::Font::bold));
    g.drawText(on ? juce::String("EN MARCHE") : u8("COUPÉ"), b.getLocalBounds().withTrimmedLeft(48), juce::Justification::centredLeft);
}

// Onglets de bibliothèque : une police commune qui loge « Pédales (77) », au lieu d'un rétrécissement au cas par cas.
juce::Font TfLook::getTextButtonFont(juce::TextButton& b, int h)
{
    return b.getProperties()["tab"] ? juce::Font(juce::FontOptions(11.5f)) : LookAndFeel_V4::getTextButtonFont(b, h);
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
    defs.add({ u8("Pédale"), "NAM", "pedal_on", {}, { "pedal_in", "pedal_out" }, { u8("Entrée (dB)"), "Sortie (dB)" } });
    defs.add({ "Ampli", "NAM", {}, {}, { "input", "output" }, { u8("Entrée (dB)"), "Sortie (dB)" } });
    defs.add({ "Baffle", "IR", "ir_on", {}, {}, {} });
    defs.add({ "Ampli B", "NAM", "ampb_on", {}, { "ampb_in", "ampb_out", "ab_mix" }, { u8("Entrée (dB)"), "Sortie (dB)", u8("Mélange A/B") } });
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
    for (auto* b : { &ampTab, &pedTab, &cabTab })
    {
        b->setClickingTogglesState(false);
        b->getProperties().set("tab", true);
        addAndMakeVisible(b);
    }
    ampTab.onClick = [this] { setTab(0); };
    pedTab.onClick = [this] { setTab(1); };
    cabTab.onClick = [this] { setTab(2); };
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
    presetBtn.onClick = [this] { blockMenu(selected); };
    tapBtn.onClick = [this] { tap(); };
    addAndMakeVisible(presetBtn);
    addChildComponent(tapBtn);
    tunerBtn.setClickingTogglesState(false);
    tunerBtn.onClick = [this] { showTuner(!proc.tunerOn); };
    addAndMakeVisible(tunerBtn);

    // Son IA
    aiBtn.setButtonText(u8("✦ Son IA"));
    aiBtn.setClickingTogglesState(false);
    aiBtn.onClick = [this]
    {
        aiOpen = !aiOpen;
        aiBtn.setToggleState(aiOpen, juce::dontSendNotification);
        resized();
        if (aiOpen)
            aiPrompt.grabKeyboardFocus();
    };
    addAndMakeVisible(aiBtn);
    aiGo.setButtonText(u8("Générer le son"));
    aiGo.onClick = [this] { askAi(); };
    aiClose.onClick = [this] { aiBtn.onClick(); };
    aiPrompt.setMultiLine(true, true);
    aiPrompt.setReturnKeyStartsNewLine(false);
    aiPrompt.onReturnKey = [this] { askAi(); };
    aiPrompt.setFont(juce::FontOptions(16.f));
    aiPrompt.setTextToShowWhenEmpty(u8("Décris le son voulu : « Metallica Black Album », « basse ronde style Motown », "
                                       "« clean brillant avec un peu d'écho »..."), tf::dim);
    aiOut.setMultiLine(true, true);
    aiOut.setReadOnly(true);
    aiOut.setCaretVisible(false);
    aiOut.setFont(juce::FontOptions(14.f));
    for (auto* c : { (juce::Component*) &aiPrompt, (juce::Component*) &aiGo, (juce::Component*) &aiClose, (juce::Component*) &aiOut })
        addChildComponent(c);

    scanLibrary();
    proc.apvts.state.addListener(this);
    selectBlock(selected);
    refreshLabels();
    setSize(1200, 720);
    startTimerHz(30);
}

ToneForgeEditor::~ToneForgeEditor()
{
    aiJob.reset();   // coupe une requête IA en cours et attend son fil
    proc.tunerOn = false;   // interface fermée : ne pas laisser le son coupé
    proc.apvts.state.removeListener(this);
    stopTimer();
    setLookAndFeel(nullptr);
}

//==============================================================================
void ToneForgeEditor::scanLibrary()
{
    auto scan = [](const char* sub, const char* pattern, bool pedal = false)
    {
        juce::Array<Entry> out;
        for (const auto& f : kRoot.getChildFile(sub).findChildFiles(juce::File::findFiles, true, pattern))
        {
            const auto grp = f.getParentDirectory().getFileName(), nm = f.getFileNameWithoutExtension();
            out.add({ f, grp, nm, brandOf(grp + " " + nm, pedal) });
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
    tagMap.clear();
    if (auto* o = juce::JSON::parse(kRoot.getChildFile("tags.json")).getDynamicObject())
        for (const auto& kv : o->getProperties())
            if (auto* a = kv.value.getArray())
                for (const auto& t : *a)
                    tagMap[kv.name.toString()].add(t.toString());
    models = scan("models", "*.nam");
    pedals = scan("pedals", "*.nam", true);
    irs = scan("irs", "*.wav");
    rebuildBrands();
}

void ToneForgeEditor::setTab(int t)
{
    if (t == tab)
        return;
    tab = t;
    brandFilter.clear();
    rebuildBrands();
}

// Un bouton par marque présente dans l'onglet courant (déjà triées par marque).
void ToneForgeEditor::rebuildBrands()
{
    brandBtns.clear();
    juce::StringArray names { "Toutes" };
    for (const auto& e : lib())
        names.addIfNotAlreadyThere(e.brand);
    if (!names.contains(brandFilter))
        brandFilter.clear();
    for (const auto& n : names)
    {
        auto* b = brandBtns.add(new juce::TextButton(n));
        b->setClickingTogglesState(false);
        b->setToggleState(n == "Toutes" ? brandFilter.isEmpty() : n == brandFilter, juce::dontSendNotification);
        b->onClick = [this, n]
        {
            brandFilter = n == "Toutes" ? juce::String() : n;
            for (auto* o : brandBtns)
                o->setToggleState(o->getButtonText() == (brandFilter.isEmpty() ? juce::String("Toutes") : brandFilter), juce::dontSendNotification);
            filterList();
            // Charge la 1re capture de la marque, sauf si celle en cours en est déjà
            const auto cur = current(target(tab));
            const auto& src = lib();
            if (brandFilter.isNotEmpty() && std::none_of(src.begin(), src.end(), [&](const Entry& e) { return e.file == cur && e.brand == brandFilter; }))
                for (const auto& e : shown)   // parmi les captures visibles (recherche comprise)
                    if (e.file != juce::File())
                    {
                        load(e.file, target(tab));
                        break;
                    }
        };
        addAndMakeVisible(b);
    }
    filterList();
    resized();
}

void ToneForgeEditor::filterList()
{
    const auto q = search.getText().trim();
    shown.clearQuick();
    int count = 0;
    juce::String last;
    for (const auto& e : lib())
        if ((brandFilter.isEmpty() || e.brand == brandFilter)
            && (q.isEmpty() || (e.group + " " + e.name + " " + e.brand + " " + tagsOf(e.file).joinIntoString(" ")).containsIgnoreCase(q)))
        {
            if (e.brand != last)
                shown.add({ {}, {}, e.brand, e.brand });   // en-tête de marque
            last = e.brand;
            shown.add(e);
            ++count;
        }
    ampTab.setToggleState(tab == 0, juce::dontSendNotification);
    pedTab.setToggleState(tab == 1, juce::dontSendNotification);
    cabTab.setToggleState(tab == 2, juce::dontSendNotification);
    ampTab.setButtonText("Amplis (" + juce::String(models.size()) + ")");
    pedTab.setButtonText(u8("Pédales (") + juce::String(pedals.size()) + ")");
    cabTab.setButtonText("Baffles (" + juce::String(irs.size()) + ")");
    countLbl.setText(juce::String(count) + (tab == 2 ? " IR" : " captures"), juce::dontSendNotification);
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
        g.drawText(upper(e.name), t, juce::Justification::centredLeft, true);
        g.setColour(bc.withAlpha(0.35f));
        g.fillRect(t.getX(), t.getBottom() - 1, t.getWidth(), 1);
        return;
    }
    const bool isCurrent = e.file == current(target(tab));
    const auto r = juce::Rectangle<int>(0, 0, w, h).toFloat().reduced(3.f, 2.f);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff272b32), 0, r.getY(), juce::Colour(0xff1b1e23), 0, r.getBottom(), false));
    g.fillRoundedRectangle(r, 7.f);
    if (isCurrent)
    {
        g.setColour(tf::acc.withAlpha(0.12f));
        g.fillRoundedRectangle(r, 7.f);
    }
    g.setColour(isCurrent ? tf::acc : juce::Colours::black.withAlpha(0.6f));
    g.drawRoundedRectangle(r, 7.f, isCurrent ? 1.6f : 1.f);
    g.setColour(bc);
    g.fillRoundedRectangle(r.withWidth(4.f), 2.f);

    const auto badge = juce::Rectangle<float>(28.f, 28.f).withCentre({ r.getX() + 24.f, r.getCentreY() });
    g.setGradientFill(juce::ColourGradient(bc.brighter(0.3f), badge.getX(), badge.getY(), bc.darker(0.45f), badge.getRight(), badge.getBottom(), false));
    g.fillEllipse(badge);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawEllipse(badge, 1.f);
    g.setColour(bc.getPerceivedBrightness() > 0.6f ? tf::ink : juce::Colours::white);
    g.setFont(juce::FontOptions(11.f, juce::Font::bold));
    g.drawText(upper(e.brand.substring(0, 2)), badge.toNearestInt(), juce::Justification::centred);

    auto t = r.withTrimmedLeft(44.f).reduced(4.f, 4.f).toNearestInt();
    if (isCurrent)
        led(g, juce::Rectangle<float>(8.f, 8.f).withCentre({ (float) t.getRight() - 4.f, r.getCentreY() }), tf::amber, true);
    if (isCurrent)
        t.removeFromRight(16);
    g.setColour(tf::txt);
    g.setFont(juce::FontOptions(13.f, juce::Font::bold));
    g.drawText(e.name, t.removeFromTop(19), juce::Justification::centredLeft, true);
    g.setFont(11.f);
    if (const auto tg = tagsOf(e.file); !tg.isEmpty())
    {
        const auto s = "#" + tg.joinIntoString(" #");
        g.setColour(tf::acc2);
        g.drawText(s, t.removeFromRight(juce::jmin(t.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), s) + 4)),
                   juce::Justification::centredRight, true);
    }
    g.setColour(tf::dim);
    g.drawText(e.group, t, juce::Justification::centredLeft, true);
}

void ToneForgeEditor::listBoxItemClicked(int row, const juce::MouseEvent& m)
{
    if (m.mods.isPopupMenu())
        return tagMenu(row);
    if (juce::isPositiveAndBelow(row, shown.size()) && shown[row].file != juce::File())
        load(shown[row].file, target(tab));
}

void ToneForgeEditor::load(const juce::File& f, int kind)
{
    if (f == current(kind))
    {
        selectBlock(blockOf(kind));   // déjà chargé : pas de rechargement
        return;
    }
    const auto err = kind == 2 || kind == 4 ? proc.loadIR(f, kind == 4) : proc.loadModel(f, kind == 1, kind == 3);
    dirty = true;
    if (err.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "ToneForge", err);
    else if (kind == 1 && !blockOn(kPedal))   // choisir une pédale l'allume
        toggleBlock(kPedal);
    else if (kind == 3 && !blockOn(kAmpB))   // choisir un ampli B l'allume
        toggleBlock(kAmpB);
    selectBlock(blockOf(kind));
    refreshLabels();
    list.repaint();
}

void ToneForgeEditor::pick()
{
    const int kind = target(tab);
    const bool isModel = tab != 2;
    chooser = std::make_unique<juce::FileChooser>(isModel ? "Capture NAM" : "IR de baffle",
                                                  kRoot.getChildFile(tab == 0 ? "models" : tab == 1 ? "pedals" : "irs"),
                                                  isModel ? "*.nam" : "*.wav");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, kind](const juce::FileChooser& fc)
                         {
                             if (fc.getResult() != juce::File())
                                 load(fc.getResult(), kind);
                         });
}

void ToneForgeEditor::refreshLabels()
{
    auto name = [](const juce::File& f) { return f == juce::File() ? juce::String("(aucun)") : f.getFileNameWithoutExtension(); };
    const bool ped = selected == kPedal, ab = selected == kAmpB;   // l'écran montre la pédale / l'ampli B quand son bloc est affiché
    nameLbl.setText(name(ped ? proc.pedalFile : ab ? proc.modelBFile : proc.modelFile), juce::dontSendNotification);
    warnLbl.setText(proc.modelWarning(ped ? 1 : ab ? 3 : 0), juce::dontSendNotification);
    if (selected == kAmp)
        panelInfo.setText(proc.modelFile.getFullPathName(), juce::dontSendNotification);
    else if (ped)
        panelInfo.setText(proc.pedalFile == juce::File() ? u8("Aucune pédale : choisis-en une dans Pédales.")
                                                         : proc.pedalFile.getFullPathName(), juce::dontSendNotification);
    else if (selected == kCab)
        panelInfo.setText(proc.irFile == juce::File() ? juce::String("Aucune IR : choisis-en une dans Baffles.")
                                                      : proc.irFile.getFullPathName(), juce::dontSendNotification);
    else if (ab)
        panelInfo.setText(proc.modelBFile == juce::File() ? u8("Choisis l'ampli B dans Amplis et son IR dans Baffles (bloc Ampli B affiché).")
                                                          : proc.modelBFile.getFullPathName() + "\n"
                                                                + (proc.irBFile == juce::File() ? juce::String("(sans IR)") : proc.irBFile.getFullPathName()),
                          juce::dontSendNotification);
    else
        panelInfo.setText(defs[selected].info, juce::dontSendNotification);
}

//==============================================================================
bool ToneForgeEditor::blockOn(int i) const
{
    const auto& d = defs.getReference(i);
    if ((i == kPedal && proc.pedalFile == juce::File()) || (i == kAmpB && proc.modelBFile == juce::File()))
        return false;
    if (d.onId.isNotEmpty())
        return proc.apvts.getRawParameterValue(d.onId)->load() > 0.5f;
    if (i == kAmp)
        return proc.modelFile != juce::File();
    return true;
}

void ToneForgeEditor::toggleBlock(int i)
{
    if (i == kPedal && proc.pedalFile == juce::File())   // rien à allumer : ouvrir la liste des pédales
        return selectBlock(kPedal);
    if (i == kAmpB && proc.modelBFile == juce::File())   // idem : ouvrir la liste des amplis pour B
        return selectBlock(kAmpB);
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
    if (i == kAmp || i == kPedal || i == kCab)   // la liste suit le bloc NAM / IR affiché
        setTab(i == kAmp ? 0 : i == kPedal ? 1 : 2);
    else if (i == kAmpB && tab == 1)
        setTab(0);
    list.repaint();   // la capture en cours change entre A et B
    const auto& d = defs.getReference(i);
    panelTitle.setText(upper(d.title), juce::dontSendNotification);
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
        if (d.ids[k] == "delay_0")
        {
            kn->slider.textFromValueFunction = [this](double v) { return juce::String(juce::roundToInt(delayMs(v, proc.getSampleRate()))) + " ms"; };
            kn->slider.valueFromTextFunction = [this](const juce::String& t) { return delayNorm(t.getDoubleValue(), proc.getSampleRate()); };
        }
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
    if (index == kAmp)   // ampli : tolex noir à liseré or
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
    g.drawText(upper(d.kind), r.withTrimmedTop(21.f).withHeight(12.f).toNearestInt(), juce::Justification::centred, true);
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
    if (e.mods.isPopupMenu())
        return ed.blockMenu(index);
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

    // Capture/IR chargée d'ailleurs (OSC, état restauré) : affichage à jour
    if (const int ls = proc.loadSerial.load(); ls != seenLoad)
    {
        seenLoad = ls;
        dirty = true;
        refreshLabels();
        list.repaint();
        for (auto& b : blocks)
            b->repaint();
    }

    // Toutes les 2 s : LED à jour et réglages sauvegardés s'ils ont changé (l'appli autonome ne sauve sinon qu'à la fermeture).
    if (++ticks % 60 == 0)
    {
        for (auto& b : blocks)
            b->repaint();
        warnLbl.setText(proc.modelWarning(selected == kPedal ? 1 : selected == kAmpB ? 3 : 0), juce::dontSendNotification);   // le taux du projet a pu changer
       #if JucePlugin_Build_Standalone
        if (auto* h = juce::StandalonePluginHolder::getInstance(); h && std::exchange(dirty, false))
            h->savePluginState();
       #endif
    }
    if (proc.tunerOn && ticks % 3 == 0)   // 10 mesures par seconde
        updateTuner();
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

    // Façade d'ampli habillée selon la marque de la capture : tolex, liseré, plaque, grille, logo
    const auto brand = proc.modelFile == juce::File() ? juce::String()
                     : brandOf(proc.modelFile.getParentDirectory().getFileName() + " " + proc.modelFile.getFileNameWithoutExtension());
    const auto sk = skinFor(brand);
    const auto pa = panelArea.toFloat();
    juce::DropShadow(Colours::black.withAlpha(0.7f), 18, { 0, 6 }).drawForRectangle(g, panelArea);
    juce::Path amp;
    amp.addRoundedRectangle(pa, 12.f);
    textured(g, amp, sk.tolex, grain);
    g.setColour(Colours::black);
    g.strokePath(amp, juce::PathStrokeType(1.5f));
    g.setColour(sk.piping.withAlpha(0.85f));
    g.drawRoundedRectangle(pa.reduced(7.f), 8.f, 2.5f);

    const auto pl = plateArea.toFloat();
    const auto plateCol = selected == kAmp ? sk.plate : pedalColour(selected).interpolatedWith(Colour(0xffd3d6db), 0.62f);
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

    // Sérigraphie lisible sur plaque claire ou sombre
    const auto ink = plateCol.getPerceivedBrightness() > 0.5f ? tf::ink : tf::cream;
    panelTitle.setColour(juce::Label::textColourId, ink);
    bypass.setColour(juce::ToggleButton::textColourId, ink);
    for (auto& k : knobs)
    {
        k->label.setColour(juce::Label::textColourId, ink);
        k->slider.setColour(juce::Slider::textBoxTextColourId, ink);
    }

    const auto gr = grilleArea.toFloat();
    grille(g, gr, selected == kCab ? Colour(0xff3a2c1c) : sk.grille);
    g.setColour(Colours::black);
    g.drawRect(gr, 1.5f);
    const auto logoArea = gr.withTrimmedBottom(40.f).toNearestInt();
    const bool brandLogo = selected == kAmp && brand.isNotEmpty() && brand != "Autres";
    const auto pedBrand = proc.pedalFile == juce::File() ? juce::String()
                        : brandOf(proc.pedalFile.getParentDirectory().getFileName() + " " + proc.pedalFile.getFileNameWithoutExtension(), true);
    const auto text = brandLogo ? brand
                    : selected == kPedal && pedBrand.isNotEmpty() && pedBrand != "Autres" ? pedBrand : defs[selected].title;
    const auto lc = brandLogo ? sk.logo : Colour(0xfff4f1e8);
    g.setFont(brandLogo ? juce::FontOptions(sk.font, sk.size, sk.italic ? juce::Font::bold | juce::Font::italic : juce::Font::bold)
                        : juce::FontOptions("Segoe Script", 56.f, juce::Font::bold));
    g.setColour(Colours::black.withAlpha(0.75f));
    g.drawText(text, logoArea + juce::Point<int>(3, 4), juce::Justification::centred);
    g.setGradientFill(juce::ColourGradient(lc.brighter(0.3f), 0, (float) logoArea.getCentreY() - 24.f, lc.darker(0.3f), 0, (float) logoArea.getCentreY() + 24.f, false));
    g.drawText(text, logoArea, juce::Justification::centred);
    repaint();   // le décor a changé : toute la fenêtre, pas seulement les composants
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
        aiBtn.setBounds(juce::Rectangle<int>(meterIn.getX() - lcdArea.getRight() - 48, 34)
                            .withCentre({ (lcdArea.getRight() + meterIn.getX()) / 2, topBar.getCentreY() }));
        tunerBtn.setBounds(juce::Rectangle<int>(130, 34).withCentre({ lcdArea.getX() - 85, topBar.getCentreY() }));
    }

    auto side = r.removeFromLeft(260).reduced(12);
    search.setBounds(side.removeFromTop(34));
    side.removeFromTop(8);
    auto tabs = side.removeFromTop(28);
    const int tw = (tabs.getWidth() - 8) / 3;
    ampTab.setBounds(tabs.removeFromLeft(tw));
    cabTab.setBounds(tabs.removeFromRight(tw));
    pedTab.setBounds(tabs.reduced(4, 0));
    side.removeFromTop(6);
    auto tools = side.removeFromTop(26);
    countLbl.setBounds(tools.removeFromLeft(80));
    fileBtn.setBounds(tools.removeFromRight(66));
    tools.removeFromRight(6);
    rescanBtn.setBounds(tools.removeFromRight(84));
    side.removeFromTop(6);
    {
        // Boutons de marque en lignes qui se replient
        int x = side.getX(), y = side.getY();
        for (auto* b : brandBtns)
        {
            const int bw = juce::GlyphArrangement::getStringWidthInt(juce::FontOptions(13.f), b->getButtonText()) + 18;
            if (x > side.getX() && x + bw > side.getRight())
            {
                x = side.getX();
                y += 26;
            }
            b->setBounds(x, y, juce::jmin(bw, side.getWidth()), 22);
            x += bw + 4;
        }
        if (!brandBtns.isEmpty())
            side.setTop(y + 30);
    }
    list.setBounds(side);

    auto c = r.reduced(16);
    chainArea = c.removeFromTop(118);
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
    presetBtn.setBounds(head.removeFromRight(110).reduced(0, 2));
    head.removeFromRight(8);
    tapBtn.setBounds(head.removeFromRight(90).reduced(0, 2));
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

    // Panneau IA par-dessus la façade
    for (auto& k : knobs)
    {
        k->slider.setVisible(!aiOpen);
        k->label.setVisible(!aiOpen);
    }
    panelTitle.setVisible(!aiOpen);
    panelInfo.setVisible(!aiOpen);
    bypass.setVisible(!aiOpen && defs[selected].onId.isNotEmpty());
    presetBtn.setVisible(!aiOpen);
    tapBtn.setVisible(!aiOpen && blockKey(selected) == "delay");
    for (auto* c : { (juce::Component*) &aiPrompt, (juce::Component*) &aiGo, (juce::Component*) &aiClose, (juce::Component*) &aiOut })
    {
        c->setVisible(aiOpen);
        c->toFront(false);
    }
    {
        auto a = plateArea.reduced(26, 22);
        auto btns = a.removeFromBottom(36);
        aiClose.setBounds(btns.removeFromRight(100));
        btns.removeFromRight(10);
        aiGo.setBounds(btns.removeFromRight(180));
        a.removeFromBottom(10);
        aiPrompt.setBounds(a);
        aiOut.setBounds(grilleArea.reduced(16));
    }

    renderBackground();
}

//==============================================================================
// Son IA : on décrit la chaîne, ses réglages et toute la bibliothèque ; l'IA répond en JSON.
void ToneForgeEditor::askAi()
{
    const auto demande = aiPrompt.getText().trim();
    if (aiBusy || demande.isEmpty())
        return;
    const auto cfg = ai::config();
    if (cfg.key.isEmpty())
    {
        aiOut.setText(u8("Aucune clé OpenRouter : ni variable OPENROUTER_API_KEY, ni C:\\dev\\dnd_dm\\data\\settings.json."));
        return;
    }

    juce::String chaine;
    for (const auto& d : defs)
    {
        chaine << "- " << d.title << " (" << d.kind << ")";
        if (d.info.isNotEmpty())
            chaine << " : " << d.info;
        chaine << "\n";
        if (d.onId.isNotEmpty())
            chaine << "    " << d.onId << " = 1 actif / 0 coupe (actuel "
                   << juce::String(proc.apvts.getRawParameterValue(d.onId)->load(), 0) << ")\n";
        for (int k = 0; k < d.ids.size(); ++k)
        {
            const auto r = proc.apvts.getParameterRange(d.ids[k]);
            chaine << "    " << d.ids[k] << " = " << d.labels[k] << ", de " << r.start << " a " << r.end
                   << " (actuel " << juce::String(proc.apvts.getRawParameterValue(d.ids[k])->load(), 2) << ")\n";
        }
    }
    // Une ligne d'en-tête par marque | dossier, puis « id nom » : beaucoup moins de jetons par requête.
    juce::String biblio;
    auto add = [&biblio](const juce::String& title, const char* prefix, const juce::Array<Entry>& src)
    {
        biblio << title;
        juce::String last;
        for (int i = 0; i < src.size(); ++i)
        {
            const auto head = src[i].brand + " | " + src[i].group;
            if (head != last)
                biblio << "# " << (last = head) << "\n";
            biblio << prefix << i << " " << src[i].name << "\n";
        }
    };
    add("Captures d'ampli NAM (# marque | dossier) :\n", "M", models);
    add(u8("\nCaptures de pédale NAM (avant l'ampli) :\n"), "P", pedals);
    add("\nIR de baffle :\n", "I", irs);

    const auto system = u8(
        "Tu es un ingénieur du son expert en amplis guitare et basse. Le musicien décrit un son (groupe, album, style "
        "ou exigence). Tu configures le simulateur ToneForge avec UNIQUEMENT les captures, IR et paramètres listés. "
        "Chaîne du signal, dans l'ordre :\n") + chaine
        + "\n" + biblio
        + u8("\nRéponds UNIQUEMENT par un objet JSON, sans texte autour :\n"
             "{\"capture\": \"M12\", \"pedale\": \"P2\", \"ir\": \"I3\", \"reglages\": {\"id_parametre\": valeur}, \"explication\": \"...\"}\n"
             "- capture / pedale / ir : un identifiant de la liste, ou null pour garder l'actuel ; pedal_on = 0 pour jouer sans pédale.\n"
             "- reglages : seulement les paramètres à changer, valeurs dans les bornes ; active ou coupe les blocs avec les *_on.\n"
             "- explication : en français, 3 à 6 phrases : pourquoi ces choix, et ce qui manque si la bibliothèque n'a "
             "pas la capture idéale (quel ampli ou quelle pédale chercher sur TONE3000). Nomme le matériel, "
             "jamais les identifiants (M12, P2, I3...).");

    aiBusy = true;
    aiGo.setEnabled(false);
    aiOut.setText(u8("Génération du son avec ") + cfg.model + "...");
    aiJob = std::make_unique<ai::Job>(cfg, system, demande,
        [sp = juce::Component::SafePointer<ToneForgeEditor>(this)](const juce::String& reply, const juce::String& err)
        {
            if (sp != nullptr)
                sp->applyAi(reply, err);
        });
}

void ToneForgeEditor::applyAi(const juce::String& reply, const juce::String& err)
{
    aiBusy = false;
    aiGo.setEnabled(true);
    if (err.isNotEmpty())
    {
        aiOut.setText(err);
        return;
    }
    // Le JSON peut être entouré de texte ou d'un bloc de code
    const auto j = juce::JSON::parse(reply.fromFirstOccurrenceOf("{", true, false).upToLastOccurrenceOf("}", true, false));
    if (!j.isObject())
    {
        aiOut.setText(u8("Réponse illisible de l'IA :\n") + reply);
        return;
    }
    juce::String recap;
    auto pickFrom = [&](const juce::var& id, const juce::Array<Entry>& src, int kind)
    {
        const auto s = id.toString().trim();
        const int i = s.getTrailingIntValue();
        if (s.startsWithIgnoreCase(kind == 0 ? "M" : kind == 1 ? "P" : "I") && juce::isPositiveAndBelow(i, src.size()))
        {
            const auto e = src[i];   // copie : load() peut toucher aux listes
            load(e.file, kind);
            recap << (kind == 0 ? juce::String("Capture : ") : kind == 1 ? u8("Pédale : ") : juce::String("IR : ")) << e.name << "\n";
        }
    };
    pickFrom(j["ir"], irs, 2);
    pickFrom(j["pedale"], pedals, 1);
    pickFrom(j["capture"], models, 0);   // en dernier : on revient sur le bloc Ampli

    if (auto* o = j["reglages"].getDynamicObject())
        for (const auto& kv : o->getProperties())
            if (auto* p = proc.apvts.getParameter(kv.name.toString()))
            {
                const auto r = proc.apvts.getParameterRange(kv.name.toString());
                const auto v = juce::jlimit(r.start, r.end, (float) (double) kv.value);
                p->beginChangeGesture();
                p->setValueNotifyingHost(r.convertTo0to1(v));
                p->endChangeGesture();
                // Lisible : interrupteurs en clair, nombres à 2 décimales au plus (-45, 0.55, 3.5)
                juce::String txt;
                if (dynamic_cast<juce::AudioParameterBool*>(p) != nullptr)
                    txt = v > 0.5f ? juce::String("en marche") : u8("coupé");
                else
                {
                    txt = juce::String(v, 2);
                    if (txt.containsChar('.'))
                        txt = txt.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
                }
                recap << p->getName(40) << " = " << txt << "\n";
            }
            else
                recap << u8("Ignoré (inconnu) : ") << kv.name.toString() << "\n";
    dirty = true;
    for (auto& b : blocks)
        b->repaint();
    // Filet : retire les identifiants internes « (M12) » que le modèle glisserait malgré la consigne.
    static const std::regex ids(R"(\s*\(\s*[MPI]\d+(\s*[,/]\s*[MPI]\d+)*\s*\))");
    const auto why = std::regex_replace(j["explication"].toString().toStdString(), ids, "");
    aiOut.setText(juce::String::fromUTF8(why.c_str()) + "\n\n" + recap);
}

//==============================================================================
// Réglages par bloc, comme les « Model Preset » (.at5m) et Copier/Coller d'AmpliTube.
juce::String ToneForgeEditor::blockKey(int i) const
{
    const auto& on = defs.getReference(i).onId;
    return on.isNotEmpty() ? on.upToFirstOccurrenceOf("_", false, false) : i == kAmp ? juce::String("amp") : juce::String("eq");
}

static int kindOf(int block, int pedal, int amp, int cab, int ampB) { return block == amp ? 0 : block == pedal ? 1 : block == cab ? 2 : block == ampB ? 3 : -1; }

juce::ValueTree ToneForgeEditor::captureBlock(int i) const
{
    const auto& d = defs.getReference(i);
    juce::ValueTree v(blockKey(i));
    auto ids = d.ids;
    if (d.onId.isNotEmpty())
        ids.add(d.onId);
    for (auto& id : ids)
        if (auto* p = proc.apvts.getParameter(id))
            v.setProperty(juce::Identifier(id), p->convertFrom0to1(p->getValue()), nullptr);
    const int kind = kindOf(i, kPedal, kAmp, kCab, kAmpB);
    if (kind >= 0 && proc.loadedFile(kind) != juce::File())
        v.setProperty("file", proc.loadedFile(kind).getFullPathName(), nullptr);
    if (kind == 3 && proc.irBFile != juce::File())
        v.setProperty("irB", proc.irBFile.getFullPathName(), nullptr);
    return v;
}

void ToneForgeEditor::applyBlock(int i, const juce::ValueTree& v)
{
    if (!v.hasType(juce::Identifier(blockKey(i))))
        return;
    const int kind = kindOf(i, kPedal, kAmp, kCab, kAmpB);
    const juce::File f(v.getProperty("file").toString());
    if (kind >= 0 && f.existsAsFile())
        load(f, kind);   // d'abord la capture : charger une pédale l'allume
    if (const juce::File ir(v.getProperty("irB").toString()); kind == 3 && ir.existsAsFile())
        load(ir, 4);
    for (int k = 0; k < v.getNumProperties(); ++k)
    {
        const auto id = v.getPropertyName(k);
        if (auto* p = proc.apvts.getParameter(id.toString()))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1((float) v.getProperty(id)));
            p->endChangeGesture();
        }
    }
    dirty = true;
    selectBlock(i);
}

void ToneForgeEditor::blockMenu(int i)
{
    const auto key = blockKey(i);
    const auto dir = kRoot.getChildFile("blocks").getChildFile(key);
    const auto files = dir.findChildFiles(juce::File::findFiles, false, "*.tfb", juce::File::FollowSymlinks::no);
    juce::PopupMenu m, sub;
    m.addSectionHeader(upper(defs[i].title));
    m.addItem(u8("Copier les réglages"), [this, i] { clip = captureBlock(i); });
    m.addItem(u8("Coller les réglages"), clip.hasType(juce::Identifier(key)), false, [this, i] { applyBlock(i, clip); });
    m.addSeparator();
    m.addItem(u8("Sauver comme preset de bloc..."), [this, i, dir]
    {
        auto* w = new juce::AlertWindow(u8("Preset de bloc"), u8("Nom du preset (") + defs[i].title + ")", juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", {});
        w->addButton("Sauver", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Annuler", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, i, dir, w](int r)
        {
            const auto name = juce::File::createLegalFileName(w->getTextEditorContents("name").trim());
            if (r != 1 || name.isEmpty())
                return;
            dir.createDirectory();
            if (auto xml = captureBlock(i).createXml())
                if (!xml->writeTo(dir.getChildFile(name + ".tfb")))
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "ToneForge",
                                                           u8("Écriture impossible : ") + dir.getFullPathName());
        }), true);
    });
    for (auto& f : files)
        sub.addItem(f.getFileNameWithoutExtension(), [this, i, f]
        {
            if (auto xml = juce::XmlDocument::parse(f))
                applyBlock(i, juce::ValueTree::fromXml(*xml));
        });
    m.addSubMenu("Charger un preset", sub, !files.isEmpty());
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(blocks[(size_t) i].get()));
}

// Tap Tempo (modèle : MIDI.Tap_Tempo d'AmpliTube) : moyenne des 4 derniers intervalles -> temps du delay.
void ToneForgeEditor::tap()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (!taps.isEmpty() && now - taps.getLast() > 2000.0)
        taps.clear();
    taps.add(now);
    if (taps.size() > 5)
        taps.remove(0);
    if (taps.size() < 2)
        return tapBtn.setButtonText("Tap...");
    const double ms = (taps.getLast() - taps.getFirst()) / (taps.size() - 1);
    if (auto* p = proc.apvts.getParameter("delay_0"))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost((float) delayNorm(ms, proc.getSampleRate()));
        p->endChangeGesture();
    }
    tapBtn.setButtonText(juce::String(juce::roundToInt(60000.0 / ms)) + " BPM");
}

//==============================================================================
// Accordeur (modèle : emplacement Tuner d'AmpliTube, avant toute la chaîne) : son coupé, note et écart sur l'écran LCD.
void ToneForgeEditor::showTuner(bool on)
{
    proc.tunerOn = on;
    tunerBtn.setToggleState(on, juce::dontSendNotification);
    nameLbl.setColour(juce::Label::textColourId, tf::amber);
    warnLbl.setColour(juce::Label::textColourId, tf::amber.withAlpha(0.75f));
    if (on)
    {
        nameLbl.setText("--", juce::dontSendNotification);
        warnLbl.setText(u8("Accordeur : son coupé"), juce::dontSendNotification);
    }
    else
        refreshLabels();
}

void ToneForgeEditor::updateTuner()
{
    // Signal moyenné vers ~24 kHz : précis à ±1,5 cent jusqu'à 1 kHz, 4x moins de calcul qu'à 48 kHz.
    const double rate = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
    const int dec = juce::jmax(1, juce::roundToInt(rate / 24000.0));
    const double sr = rate / dec;
    proc.readTuner(tunerBuf, juce::jmin(1024 + (int) (sr / 28.0) + 2, 32768 / dec), dec);
    const float f = yinPitch(tunerBuf, sr);
    if (f <= 0.f)
    {
        nameLbl.setColour(juce::Label::textColourId, tf::amber.withAlpha(0.5f));
        return;   // garde la dernière note affichée, estompée
    }
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const double midi = 69.0 + 12.0 * std::log2(f / 440.0);
    const int note = juce::roundToInt(midi);
    const int cents = juce::roundToInt((midi - note) * 100.0);
    const bool inTune = std::abs(cents) <= 3;
    nameLbl.setText(juce::String(names[((note % 12) + 12) % 12]) + juce::String(note / 12 - 1) + "   "
                        + (cents > 0 ? "+" : "") + juce::String(cents) + u8(" ¢"),
                    juce::dontSendNotification);
    // Aiguille en texte : 21 cases de -50 à +50 cents
    juce::String bar;
    const int pos = juce::jlimit(0, 20, juce::roundToInt((cents + 50) / 5.0));
    for (int i = 0; i <= 20; ++i)
        bar << (i == pos ? u8("█") : i == 10 ? u8("|") : u8("·"));
    warnLbl.setText(bar + "   " + juce::String(f, 1) + " Hz", juce::dontSendNotification);
    const auto col = inTune ? tf::ok : std::abs(cents) <= 15 ? tf::warn : tf::bad;
    nameLbl.setColour(juce::Label::textColourId, col);
    warnLbl.setColour(juce::Label::textColourId, col.withAlpha(0.85f));
}

//==============================================================================
// Étiquettes (modèle : Presets.db d'AmpliTube : genre, caractère…), clic droit sur une capture.
static juce::String tagKey(const juce::File& f) { return f.getRelativePathFrom(kRoot).replaceCharacter('\\', '/'); }

juce::StringArray ToneForgeEditor::tagsOf(const juce::File& f) const
{
    const auto it = tagMap.find(tagKey(f));
    return it != tagMap.end() ? it->second : juce::StringArray();
}

void ToneForgeEditor::saveTags() const
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : tagMap)
        if (!v.isEmpty())
        {
            juce::Array<juce::var> a;
            for (const auto& t : v)
                a.add(t);
            o->setProperty(k, a);
        }
    kRoot.getChildFile("tags.json").replaceWithText(juce::JSON::toString(juce::var(o)));
}

void ToneForgeEditor::tagMenu(int row)
{
    juce::StringArray all { "Clean", "Crunch", "High gain", "Metal", "Rock", "Blues", "Jazz", "Funk", "Basse",
                            "Chaud", "Brillant", "Sombre", "Favori" };
    for (const auto& [k, v] : tagMap)
        all.addArray(v);
    all.removeDuplicates(true);
    all.sortNatural();
    const bool onRow = juce::isPositiveAndBelow(row, shown.size()) && shown[row].file != juce::File();
    const auto f = onRow ? shown[row].file : juce::File();
    const auto mine = tagsOf(f);
    juce::PopupMenu m, filt;
    if (onRow)
    {
        m.addSectionHeader(shown[row].name);
        for (int i = 0; i < all.size(); ++i)
            m.addItem(1 + i, all[i], true, mine.contains(all[i]));
        m.addItem(1000, u8("Nouvelle étiquette..."));
        m.addSeparator();
    }
    juce::StringArray used;
    for (const auto& [k, v] : tagMap)
        used.addArray(v);
    used.removeDuplicates(true);
    used.sortNatural();
    for (int i = 0; i < used.size(); ++i)
        filt.addItem(2000 + i, "#" + used[i]);
    m.addSubMenu(u8("Filtrer par étiquette"), filt, !used.isEmpty());
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [this, f, all, used](int r)
    {
        auto toggle = [this, f](const juce::String& t)
        {
            auto& v = tagMap[tagKey(f)];
            if (v.contains(t))
                v.removeString(t);
            else
                v.add(t);
            saveTags();
            filterList();
        };
        if (r >= 2000)
            search.setText(used[r - 2000]);   // onTextChange refiltre
        else if (r == 1000)
        {
            auto* w = new juce::AlertWindow(u8("Nouvelle étiquette"), {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor("t", {});
            w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            w->addButton("Annuler", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true, juce::ModalCallbackFunction::create([w, toggle](int ok)
            {
                const auto t = w->getTextEditorContents("t").trim().removeCharacters("#,");
                if (ok && t.isNotEmpty())
                    toggle(t);
            }), true);
        }
        else if (r > 0)
            toggle(all[r - 1]);
    });
}
