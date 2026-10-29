#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PhoneEditor.h"
#include "OscServer.h"
#include "NAM/get_dsp.h"

// Les tampons internes font maxBlock : un bloc plus long est traité par tranches.
template <typename F>
static void chunks(int n, size_t cap, F&& f)
{
    const int c = (int) cap;
    for (int o = 0; c > 0 && o < n; o += c)
        f(o, juce::jmin(c, n - o));
}

using TP = ToneForgeProcessor;

// Codes d'unité côté audio : effets 0..2·nd-1 (k % nd = définition, k / nd = exemplaire), NAM, IR, EQ, gate, repères
enum { kNamUnit = 20, kIrUnit = 30, kEqUnit = 40, kGateUnit = 50, kSplit = 60, kBar = 61, kMix = 62 };

static int unitCode(const juce::String& nm)
{
    const auto& d = fxDefs();
    const int nd = (int) d.size();
    for (int k = 0; k < nd; ++k)
    {
        if (nm == d[(size_t) k].key)
            return k;
        if (nm == juce::String(d[(size_t) k].key) + "2")
            return k + nd;
    }
    const auto numbered = [&nm](const char* p, int count, int base)
    {
        const auto rest = nm.substring((int) std::strlen(p));
        const int i = rest.getIntValue();
        return nm.startsWith(p) && rest.containsOnly("0123456789") && i >= 1 && i <= count ? base + i - 1 : -1;
    };
    if (const int c = numbered("nam", TP::kNams, kNamUnit); c >= 0)
        return c;
    if (const int c = numbered("ir", TP::kIrs, kIrUnit); c >= 0)
        return c;
    if (const int c = numbered("eq", TP::kEqs, kEqUnit); c >= 0)
        return c;
    if (const int c = numbered("gate", TP::kGates, kGateUnit); c >= 0)
        return c;
    return nm == "split" ? kSplit : nm == "|" ? kBar : nm == "mix" ? kMix : -1;
}

// Anciens identifiants conservés (presets), le reste ajouté en fin de layout
juce::String ToneForgeProcessor::namParamId(int i, int what)
{
    if (what == 1 && i <= 1)
        return i == 0 ? "input" : "ampb_in";
    if (what == 2 && i == 1)
        return "ampb_out";
    static const char* const pfx[kNams] = { "amp", "nam2", "pedal", "pedal2", "pedal3", "pedal4", "nam7", "nam8" };
    static const char* const suf[5] = { "_on", "_in", "_out", "_tone", "_mix" };
    return juce::String(pfx[i]) + suf[what];
}
static juce::String irOnId(int i) { return i == 0 ? juce::String("ir_on") : "ir" + juce::String(i + 1) + "_on"; }
static juce::String gateId(int i) { return i == 0 ? juce::String("gate") : "gate" + juce::String(i + 1); }
static juce::String eqId(int i, int band)
{
    static const char* const b[3] = { "bass", "mid", "treble" };
    return i == 0 ? juce::String(b[band]) : "eq" + juce::String(i + 1) + "_" + b[band];
}
// Propriétés de l'état : fichiers des captures et des IR
static const char* const kNamProp[TP::kNams] = { "model", "modelB", "pedal", "pedal2", "pedal3", "pedal4", "nam7", "nam8" };
static const char* const kIrProp[TP::kIrs] = { "ir", "irB", "ir3", "ir4" };

ToneForgeProcessor::ToneForgeProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", layout())
{
    for (int copy = 0; copy < kFxCopies; ++copy)
        for (auto& d : fxDefs())
        {
            fx.emplace_back(d.make(nullptr));
            const auto key = juce::String(d.key) + (copy > 0 ? "2" : "");
            fxOn.push_back(apvts.getRawParameterValue(key + "_on"));
            fxPar.emplace_back();
            for (size_t i = 0; i < d.params.size(); ++i)
                fxPar.back().push_back(apvts.getRawParameterValue(key + "_" + juce::String((int) i)));
        }
    spare.resize(fx.size());
    spareReady.reset(new std::atomic<bool>[fx.size()]);
    for (size_t k = 0; k < fx.size(); ++k)
        spareReady[k] = false;
    lastOn.assign(fx.size(), 0);
    for (int i = 0; i < kNams; ++i)
        for (int w = 0; w < 5; ++w)
            namPar[i][w] = apvts.getRawParameterValue(namParamId(i, w));
    for (int i = 0; i < kIrs; ++i)
        irOn[i] = apvts.getRawParameterValue(irOnId(i));
    for (int i = 0; i < kGates; ++i)
    {
        gatePar[i][0] = apvts.getRawParameterValue(gateId(i) + "_on");
        gatePar[i][1] = apvts.getRawParameterValue(gateId(i));
    }
    for (int i = 0; i < kEqs; ++i)
        for (int b = 0; b < 3; ++b)
            eqPar[i][b] = apvts.getRawParameterValue(eqId(i, b));
    publishChain(defaultRoute());
    timerCallback();
    startTimer(30);   // la macro suit la molette sans saccade
    if (wrapperType == wrapperType_Standalone)   // un seul serveur : pas d'instance de scan ni de doublon dans l'hôte
        osc = std::make_unique<OscServer>(*this);
}

ToneForgeProcessor::~ToneForgeProcessor() = default;

void ToneForgeProcessor::timerCallback()
{
    const auto& defs = fxDefs();
    for (size_t k = 0; k < spare.size(); ++k)
        if (!spareReady[k].load(std::memory_order_acquire))
        {
            spare[k].reset(defs[k % defs.size()].make(nullptr));   // détruit aussi l'ancienne instance usée
            spareReady[k].store(true, std::memory_order_release);
        }
    // Latence déclarée à l'hôte : NAM avant et après la séparation + la chaîne la plus retardée (A, ou A et B si B joue)
    const bool bOn = apvts.getRawParameterValue("ampb_on")->load() > 0.5f;
    float lat[4] {};   // avant, A, B, après
    int sec = 0;
    for (auto& nm : getRoute())
    {
        const int c = unitCode(nm);
        if (c >= kSplit)
            sec = c - kSplit + 1;
        else if (c >= kNamUnit && c < kNamUnit + kNams)
            lat[sec] += namLatency(c - kNamUnit);
    }
    const int latency = juce::roundToInt(lat[0] + (bOn ? juce::jmax(lat[1], lat[2]) : lat[1]) + lat[3]);
    if (latency != getLatencySamples())
        setLatencySamples(latency);
    // Macro : applique les cibles quand sa valeur change (thread de messages, comme un geste d'interface)
    const float m = apvts.getRawParameterValue("macro")->load();
    if (m == lastMacro)
        return;
    lastMacro = m;
    juce::ValueTree targets;
    {
        const juce::ScopedLock sl(stateLock);
        targets = macroTree().createCopy();
    }
    for (const auto& t : targets)
        if (auto* p = apvts.getParameter(t["id"].toString()))
        {
            const int c = t["curve"];
            const float x = c == 1 ? m * m : c == 2 ? 1.f - (1.f - m) * (1.f - m) : m;
            const float lo = t["min"], hi = t["max"];
            p->beginChangeGesture();   // l'hôte enregistre l'automation comme un geste d'interface
            p->setValueNotifyingHost(lo + (hi - lo) * x);
            p->endChangeGesture();
        }
}

juce::AudioProcessorValueTreeState::ParameterLayout ToneForgeProcessor::layout()
{
    using P = juce::AudioParameterFloat;
    using R = juce::NormalisableRange<float>;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterBool>("gate_on", "Gate", true));
    p.push_back(std::make_unique<P>("gate", "Gate (dB)", R(-100.f, -20.f), -70.f));
    p.push_back(std::make_unique<P>("input", "Entrée (dB)", R(-24.f, 24.f), 0.f));
    p.push_back(std::make_unique<juce::AudioParameterBool>("ir_on", "Baffle (IR)", true));
    p.push_back(std::make_unique<P>("bass", "Graves (dB)", R(-12.f, 12.f), 0.f));
    p.push_back(std::make_unique<P>("mid", "Médiums (dB)", R(-12.f, 12.f), 0.f));
    p.push_back(std::make_unique<P>("treble", "Aigus (dB)", R(-12.f, 12.f), 0.f));
    p.push_back(std::make_unique<P>("output", "Sortie (dB)", R(-24.f, 12.f), 0.f));
    for (auto& d : fxDefs())
    {
        const juce::String key(d.key), title(juce::CharPointer_UTF8(d.title));
        p.push_back(std::make_unique<juce::AudioParameterBool>(key + "_on", title, false));
        const auto shortName = title.upToFirstOccurrenceOf(" (", false, false);
        for (size_t i = 0; i < d.params.size(); ++i)
            p.push_back(std::make_unique<P>(key + "_" + juce::String((int) i),
                                            shortName + " " + juce::String(juce::CharPointer_UTF8(d.params[i].label)),
                                            R(0.f, 1.f), d.params[i].def));
    }
    // Pédale NAM avant l'ampli (ajoutée en fin de liste : n'altère pas les paramètres existants)
    p.push_back(std::make_unique<juce::AudioParameterBool>("pedal_on", u8("Pédale"), true));
    p.push_back(std::make_unique<P>("pedal_in", u8("Pédale entrée (dB)"), R(-24.f, 24.f), 0.f));
    p.push_back(std::make_unique<P>("pedal_out", u8("Pédale sortie (dB)"), R(-24.f, 24.f), 0.f));
    // Ampli B en parallèle (ajouté en fin de liste)
    p.push_back(std::make_unique<juce::AudioParameterBool>("ampb_on", "Ampli B", false));
    p.push_back(std::make_unique<P>("ampb_in", u8("Ampli B entrée (dB)"), R(-24.f, 24.f), 0.f));
    p.push_back(std::make_unique<P>("ampb_out", "Ampli B sortie (dB)", R(-24.f, 24.f), 0.f));
    p.push_back(std::make_unique<P>("ab_mix", u8("Mélange A/B"), R(0.f, 1.f), 0.5f));
    p.push_back(std::make_unique<P>("macro", "Macro", R(0.f, 1.f), 0.f));
    // Attaque : la force du jeu fait glisser le mélange de A vers B (ajouté en fin de liste)
    p.push_back(std::make_unique<P>("dyn_amt", "Attaque A>B", R(0.f, 1.f), 0.f));
    p.push_back(std::make_unique<P>("dyn_thr", "Attaque seuil (dB)", R(-60.f, 0.f), -30.f));
    p.push_back(std::make_unique<P>("dyn_range", "Attaque plage (dB)", R(3.f, 36.f), 12.f));
    p.push_back(std::make_unique<P>("dyn_rel", "Attaque retour (ms)", R(20.f, 1000.f, 0.f, 0.4f), 200.f));
    // Pédales : Tone et Blend de la 1re, puis les emplacements 2 à 4 au complet (ajoutés en fin de liste)
    for (int i = 0; i < kPedals; ++i)
    {
        const auto id = pedalId(i), nm = i == 0 ? u8("Pédale") : u8("Pédale ") + juce::String(i + 1);
        if (i > 0)
        {
            p.push_back(std::make_unique<juce::AudioParameterBool>(id + "_on", nm, true));
            p.push_back(std::make_unique<P>(id + "_in", nm + u8(" entrée (dB)"), R(-24.f, 24.f), 0.f));
            p.push_back(std::make_unique<P>(id + "_out", nm + " sortie (dB)", R(-24.f, 24.f), 0.f));
        }
        p.push_back(std::make_unique<P>(id + "_tone", nm + " tone", R(0.f, 1.f), 0.5f));
        p.push_back(std::make_unique<P>(id + "_mix", nm + " blend", R(0.f, 1.f), 1.f));
    }
    // Système modulable (ajouté en fin de liste) : réserves de blocs et mélangeur des chaînes A/B
    for (int i : { 0, 1, 6, 7 })   // NAM sans jeu complet : seuls les identifiants nouveaux sont créés
    {
        const juce::String nm = i == 0 ? "Ampli A" : i == 1 ? "Ampli B" : "NAM " + juce::String(i + 1);
        const auto add = [&](int w, std::unique_ptr<juce::RangedAudioParameter> q)
        {
            if (!(w == 1 && i <= 1) && !(w == 2 && i == 1))
                p.push_back(std::move(q));
        };
        add(0, std::make_unique<juce::AudioParameterBool>(namParamId(i, 0), nm, true));
        add(1, std::make_unique<P>(namParamId(i, 1), nm + u8(" entrée (dB)"), R(-24.f, 24.f), 0.f));
        add(2, std::make_unique<P>(namParamId(i, 2), nm + " sortie (dB)", R(-24.f, 24.f), 0.f));
        add(3, std::make_unique<P>(namParamId(i, 3), nm + " tone", R(0.f, 1.f), 0.5f));
        add(4, std::make_unique<P>(namParamId(i, 4), nm + " blend", R(0.f, 1.f), 1.f));
    }
    for (int i = 1; i < kIrs; ++i)
        p.push_back(std::make_unique<juce::AudioParameterBool>(irOnId(i), "Baffle " + juce::String(i + 1), true));
    for (int i = 1; i < kGates; ++i)
    {
        p.push_back(std::make_unique<juce::AudioParameterBool>(gateId(i) + "_on", "Gate " + juce::String(i + 1), true));
        p.push_back(std::make_unique<P>(gateId(i), "Gate " + juce::String(i + 1) + " (dB)", R(-100.f, -20.f), -70.f));
    }
    for (int i = 1; i < kEqs; ++i)
        for (int b = 0; b < 3; ++b)
            p.push_back(std::make_unique<P>(eqId(i, b), "EQ " + juce::String(i + 1) + " "
                                                + (b == 0 ? u8("graves") : b == 1 ? u8("médiums") : u8("aigus")) + " (dB)",
                                            R(-12.f, 12.f), 0.f));
    for (auto& d : fxDefs())
    {
        const juce::String key = juce::String(d.key) + "2", title(juce::CharPointer_UTF8(d.title));
        const auto shortName = title.upToFirstOccurrenceOf(" (", false, false) + " 2";
        p.push_back(std::make_unique<juce::AudioParameterBool>(key + "_on", shortName, false));
        for (size_t i = 0; i < d.params.size(); ++i)
            p.push_back(std::make_unique<P>(key + "_" + juce::String((int) i),
                                            shortName + " " + juce::String(juce::CharPointer_UTF8(d.params[i].label)),
                                            R(0.f, 1.f), d.params[i].def));
    }
    for (auto c : { "a", "b" })
    {
        const juce::String up = juce::String(c).toUpperCase();
        p.push_back(std::make_unique<P>(juce::String("mix_vol") + c, u8("Mélangeur volume ") + up + " (dB)", R(-24.f, 12.f), 0.f));
        p.push_back(std::make_unique<P>(juce::String("mix_pan") + c, u8("Mélangeur pan ") + up, R(-1.f, 1.f), 0.f));
    }
    return { p.begin(), p.end() };
}

bool ToneForgeProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    auto out = l.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono())
        && !l.getMainInputChannelSet().isDisabled();
}

void ToneForgeProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    maxBlock = samplesPerBlock;
    scratch.assign((size_t) samplesPerBlock, 0.f);
    for (auto& e : fx)
        e->setSampleRate((float) sampleRate);
    bufB.assign((size_t) samplesPerBlock, 0.f);
    dryBuf.assign((size_t) samplesPerBlock, 0.f);
    dynMix.assign((size_t) samplesPerBlock, 0.f);
    dynEnv = 0.f;
    if (loopBuf.getNumSamples() != (int) (sampleRate * 60.0))   // même taux : la boucle survit à un nouveau prepare
    {
        loopBuf.setSize(2, (int) (sampleRate * 60.0));
        loopBuf.clear();
        loopState = loopEmpty;
        loopLen = loopPos = 0;
    }
    for (auto& s : nams)
    {
        const juce::SpinLock::ScopedLockType sl(s.lock);
        s.in.assign((size_t) samplesPerBlock, 0.f);
        s.out.assign((size_t) samplesPerBlock, 0.f);
        if (s.model)
            configureModel(*s.model, s);
    }
    // Gate, IR et EQ préparés en stéréo : ils peuvent aussi aller après le mélangeur
    const juce::dsp::ProcessSpec mono { sr, (juce::uint32) samplesPerBlock, 1 }, stereo { sr, (juce::uint32) samplesPerBlock, 2 };
    for (auto& g : gates)
    {
        g.prepare(stereo);
        g.setRatio(10.f);
        g.setAttack(1.f);
        g.setRelease(80.f);
    }
    for (auto& px : namX)
    {
        px.dry.prepare(mono);
        px.z1 = px.z2 = 0.f;
    }
    for (auto& c : cabs)
        c.prepare(stereo);
    alignA.prepare(mono);
    alignB.prepare(mono);
    for (int i = 0; i < kEqs; ++i)
    {
        eqs[i].prepare(stereo);
        lastEq[i][0] = 99.f;
        updateEq(i);
    }
}

void ToneForgeProcessor::runFx(size_t k, float* l, float* r, int n)
{
    {
        const bool on = fxOn[k]->load() >= 0.5f;
        if (on && !lastOn[k] && spareReady[k].load(std::memory_order_acquire))
        {
            std::swap(fx[k], spare[k]);   // repart avec des tampons vides, sans allouer ici
            fx[k]->setSampleRate((float) sr);
            spareReady[k].store(false, std::memory_order_release);
        }
        lastOn[k] = on;
        if (!on)
            return;
        for (size_t i = 0; i < fxPar[k].size(); ++i)
            fx[k]->setParameter((VstInt32) i, fxPar[k][i]->load());
        float* io[2] { l, r };
        fx[k]->processReplacing(io, io, n);
    }
}

void ToneForgeProcessor::updateEq(int i)
{
    const float b = eqPar[i][0]->load(), m = eqPar[i][1]->load(), t = eqPar[i][2]->load();
    auto& last = lastEq[i];
    if (b == last[0] && m == last[1] && t == last[2])
        return;   // recalcul seulement si un réglage a bougé
    last[0] = b; last[1] = m; last[2] = t;
    auto db = [](float v) { return juce::Decibels::decibelsToGain(v); };
    using C = juce::dsp::IIR::Coefficients<float>;
    *eqs[i].get<0>().state = *C::makeLowShelf(sr, 120.f, 0.7f, db(b));
    *eqs[i].get<1>().state = *C::makePeakFilter(sr, 800.f, 0.8f, db(m));
    *eqs[i].get<2>().state = *C::makeHighShelf(sr, 3500.f, 0.7f, db(t));
}

void ToneForgeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    auto v = [this](const char* id) { return apvts.getRawParameterValue(id)->load(); };
    auto peak = [](std::atomic<float>& a, float m) { if (m > a.load()) a.store(m); };
    const float inMag = buffer.getMagnitude(0, 0, n);
    peak(inPeak, inMag);
    peak(oscInPeak, inMag);

    if (tunerOn)
    {
        const float* in = buffer.getReadPointer(0);
        int w = ringPos.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
            ring[(w + i) & (kRing - 1)] = in[i];
        ringPos.store((w + n) & (kRing - 1), std::memory_order_release);
        buffer.clear();
        return;
    }

    // Guitare = mono : on traite le canal 0, puis on recopie vers les autres.
    float* x = buffer.getWritePointer(0);

    // Attaque : suiveur de crête sur l'entrée brute (avant gate et compresseur), attaque 1 ms, retour réglable.
    // Niveau en dB -> part de B : 0 sous le seuil, 1 à seuil + plage.
    const float dynAmt = dynMix.empty() ? 0.f : v("dyn_amt");
    if (dynAmt > 0.f)
    {
        const float att = std::exp(-1.f / (0.001f * (float) sr)),
                    rel = std::exp(-1.f / (0.001f * v("dyn_rel") * (float) sr));
        const float thr = v("dyn_thr"), range = v("dyn_range");
        const int m = juce::jmin(n, (int) dynMix.size());   // au-delà (bloc plus grand qu'annoncé) : dernière valeur
        for (int i = 0; i < n; ++i)
        {
            const float s = std::abs(x[i]);
            dynEnv = s + (s > dynEnv ? att : rel) * (dynEnv - s);
            if (i < m)
                dynMix[(size_t) i] = juce::jlimit(0.f, 1.f, (juce::Decibels::gainToDecibels(dynEnv, -100.f) - thr) / range);
        }
    }

    if (chainDirty.load(std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType sl(chainLock);
        if (sl.isLocked())
        {
            chainCur = chainPending;
            chainDirty = false;
            for (size_t k = 0; k < fx.size(); ++k)   // effet retiré : repartira à neuf s'il revient
            {
                bool in = false;
                for (int j = 0; j < chainCur.n; ++j)
                    in = in || chainCur.u[j] == (int) k;
                if (!in)
                    lastOn[k] = 0;
            }
        }
    }
    const auto& u = chainCur.u;
    const int cn = chainCur.n;
    int ci = 0;   // avant la séparation, en mono
    for (; ci < cn && u[ci] != kSplit; ++ci)
        runUnit(u[ci], x, nullptr, n);
    const int a0 = juce::jmin(ci + 1, cn);
    int bar = a0;
    while (bar < cn && u[bar] != kBar)
        ++bar;
    const int b0 = juce::jmin(bar + 1, cn);
    int mx = b0;
    while (mx < cn && u[mx] != kMix)
        ++mx;
    const bool bOn = v("ampb_on") > 0.5f;
    if (bOn && !bActive)
    {
        for (int j = b0; j < mx; ++j)   // pas de reste d'une ancienne queue de convolution au rallumage
            if (u[j] >= kIrUnit && u[j] < kIrUnit + kIrs)
                cabs[u[j] - kIrUnit].reset();
        alignA.reset();
        alignB.reset();
    }
    bActive = bOn;

    // Chaînes A et B en parallèle (modèle : séparation et mélangeur d'AmpliTube), passage en stéréo au mélangeur.
    // Balance : centre = gain 1 des deux côtés (comme l'ancienne recopie mono).
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    const auto side = [](float p, bool left) { return left ? (p > 0.f ? 1.f - p : 1.f) : (p < 0.f ? 1.f + p : 1.f); };
    const float gA = juce::Decibels::decibelsToGain(v("mix_vola")), gB = juce::Decibels::decibelsToGain(v("mix_volb")),
                pA = v("mix_pana"), pB = v("mix_panb");
    const float mix = bOn ? v("ab_mix") : 0.f;
    const float dB = bOn ? rowLatency(a0, bar) - rowLatency(b0, mx) : 0.f;   // > 0 : B attend A, < 0 : A attend B
    float shownMix = mix;
    chunks(n, bufB.size(), [&](int o, int c)
    {
        float* a = x + o;
        float* b = bufB.data();
        if (bOn)
            std::copy(a, a + c, b);
        for (int j = a0; j < bar; ++j)
            runUnit(u[j], a, nullptr, c);
        if (bOn)
        {
            for (int j = b0; j < mx; ++j)
                runUnit(u[j], b, nullptr, c);
            auto& dl = dB > 0 ? alignB : alignA;
            float* p = dB > 0 ? b : a;
            if (std::abs(dB) > 0.01f)
                for (int i = 0; i < c; ++i)
                {
                    dl.pushSample(0, p[i]);
                    p[i] = dl.popSample(0, std::abs(dB));
                }
        }
        for (int i = 0; i < c; ++i)
        {
            float m = mix;
            if (bOn && dynAmt > 0.f)   // Attaque à fond : l'enveloppe remplace le mélange fixe
                m = shownMix = mix + dynAmt * (dynMix[(size_t) juce::jmin(o + i, (int) dynMix.size() - 1)] - mix);
            const float sa = a[i] * (1.f - m) * gA, sb = bOn ? b[i] * m * gB : 0.f;
            if (right)
            {
                right[o + i] = sa * side(pA, false) + sb * side(pB, false);
                a[i] = sa * side(pA, true) + sb * side(pB, true);
            }
            else
                a[i] = sa + sb;
        }
    });
    if (bOn)
        dynPos.store(shownMix);

    // Après le mélangeur, en stéréo (canal droit fictif si la sortie est mono)
    for (ci = juce::jmin(mx + 1, cn); ci < cn; ++ci)
        runUnit(u[ci], x, right, n);
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom(ch, 0, buffer, ch % 2, 0, n);

    buffer.applyGain(juce::Decibels::decibelsToGain(v("output")));
    runLooper(buffer, n);
    const float outMag = buffer.getMagnitude(0, 0, n);
    peak(outPeak, outMag);
    peak(oscOutPeak, outMag);
}

void ToneForgeProcessor::readTuner(std::vector<float>& out, int n, int dec) const
{
    // Lecture sans verrou : un bloc à demi écrit ne fausse qu'une mesure sur dix par seconde.
    out.resize((size_t) n);
    int r = ringPos.load(std::memory_order_acquire) - n * dec;
    for (int i = 0; i < n; ++i)
    {
        float s = 0.f;
        for (int k = 0; k < dec; ++k)
            s += ring[(r++) & (kRing - 1)];
        out[(size_t) i] = s / (float) dec;
    }
}

// Capture NAM d'un emplacement ; les gains ne s'appliquent que si une capture est chargée,
// y compris pendant un remplacement (le try-lock échoue mais le niveau ne saute pas).
void ToneForgeProcessor::runNam(NamSlot& s, float* x, int n, float gainIn, float gainOut)
{
    if (!s.loaded)
        return;
    juce::FloatVectorOperations::multiply(x, gainIn, n);
    {
        const juce::SpinLock::ScopedTryLockType sl(s.lock);
        if (sl.isLocked() && s.model)
            chunks(n, s.in.size(), [&](int o, int c)
            {
                if (s.rs.on)
                    return processResampled(s.rs, s.model.get(), x + o, c);
                std::copy(x + o, x + o + c, s.in.data());
                float* in = s.in.data();
                float* out = s.out.data();
                s.model->process(&in, &out, c);
                std::copy(out, out + c, x + o);
            });
    }
    juce::FloatVectorOperations::multiply(x, gainOut, n);
}

juce::String ToneForgeProcessor::loadNam(int i, const juce::File& f)
{
    std::unique_ptr<nam::DSP> m;
    try
    {
        m = nam::get_dsp(std::filesystem::path(f.getFullPathName().toWideCharPointer()));
    }
    catch (const std::exception& e)
    {
        return juce::String("Capture illisible : ") + e.what();
    }
    if (!m)
        return "Capture illisible.";
    {
        // Configuré sous le verrou : un prepareToPlay concurrent ne peut pas laisser des tampons à l'ancienne taille.
        // Le thread audio ne fait qu'un try-lock : il saute NAM le temps du chargement, sans bloquer.
        auto& s = nams[i];
        const juce::SpinLock::ScopedLockType sl(s.lock);
        configureModel(*m, s);
        std::swap(s.model, m);
        s.loaded = true;
    }
    {
        const juce::ScopedLock fl(fileLock);
        namFiles[i] = f;
    }
    ++loadSerial;
    return {};   // l'ancien modèle est détruit ici, hors du verrou
}

// Prépare la capture au taux du projet, ou au sien avec rééchantillonnage (Lagrange, ~2 échantillons
// de latence par étage). Alloue : jamais depuis le thread audio en cours de lecture.
void ToneForgeProcessor::configureModel(nam::DSP& m, NamSlot& s)
{
    auto& r = s.rs;
    const double want = m.GetExpectedSampleRate();
    r.on = want > 0 && std::abs(want - sr) > 1 && !m.SupportsArbitrarySampleRate();
    s.captureRate = r.on ? juce::roundToInt(want) : 0;
    s.projectRate = juce::roundToInt(sr);
    r.ratio = r.on ? want / sr : 1.0;
    const int blk = (int) std::ceil(maxBlock * r.ratio) + 16;
    m.Reset(r.on ? want : sr, r.on ? blk : maxBlock);
    r.up.reset();
    r.down.reset();
    r.in.assign((size_t) maxBlock + 16, 0.f);
    r.mid.assign((size_t) blk, 0.f);
    r.out.assign((size_t) blk * 2 + 32, 0.f);
    r.inN = 0;
    // Amorce de silence : la sortie ne manque jamais d'échantillons (l'interpolateur garde 3 entrées, soit 3·ratio en sortie)
    r.outN = juce::jmax(12, (int) std::ceil(3 * r.ratio) + 8);
    // Elliptique : plat jusqu'à 0,45 du taux le plus bas, -70 dB à sa fréquence de Nyquist
    const auto setupAA = [&](Resampler& q)
    {
        q.aa.clear();
        if (!q.on)
            return;
        const double hi = juce::jmax(want, sr), lo = juce::jmin(want, sr);
        for (auto& c : juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderEllipticMethod(
                 (float) (0.475 * lo), hi, (float) (0.05 * lo / hi), -0.1f, -70.f))
            q.aa.emplace_back(c);
    };
    setupAA(r);

    // Retard réel de la chaîne (amorce + interpolateurs) : phase d'un sinus lent à travers une copie à vide.
    float lat = 0.f;
    if (r.on)
    {
        Resampler t;   // interpolateurs non copiables : copie à l'état neuf
        t.on = true;
        t.ratio = r.ratio;
        t.in = r.in;
        t.mid = r.mid;
        t.out = r.out;
        t.outN = r.outN;
        setupAA(t);
        const int len = 4096, per = 512;
        const double w = juce::MathConstants<double>::twoPi / per;
        std::vector<float> buf((size_t) len);
        for (int i = 0; i < len; ++i)
            buf[(size_t) i] = (float) std::sin(w * i);
        for (int o = 0; o < len; o += maxBlock)
            processResampled(t, nullptr, buf.data() + o, juce::jmin(maxBlock, len - o));
        double sn = 0, cs = 0;
        for (int i = len / 2; i < len; ++i)
        {
            sn += buf[(size_t) i] * std::sin(w * i);
            cs += buf[(size_t) i] * std::cos(w * i);
        }
        lat = (float) (std::atan2(-cs, sn) / w);
    }
    s.latency = lat;
}

void ToneForgeProcessor::processResampled(Resampler& r, nam::DSP* dsp, float* x, int n)
{
    const auto lowpass = [&r](float* p, int k)
    {
        for (auto& f : r.aa)
            for (int i = 0; i < k; ++i)
                p[i] = f.processSample(p[i]);
    };
    std::copy(x, x + n, r.in.data() + r.inN);
    if (r.ratio < 1)
        lowpass(r.in.data() + r.inN, n);   // capture plus lente : filtrer avant la descente vers son taux
    r.inN += n;

    // Montée au taux de la capture : on garde 3 échantillons d'avance pour l'interpolateur.
    const int m = juce::jlimit(0, (int) r.mid.size(),
                               juce::jmin((int) ((r.inN - 3) * r.ratio), (int) r.out.size() - r.outN));
    const int used = r.up.process(1.0 / r.ratio, r.in.data(), r.mid.data(), m, r.inN, 0);
    r.inN -= used;
    std::move(r.in.begin() + used, r.in.begin() + used + r.inN, r.in.begin());

    float* in = r.mid.data();
    float* out = r.out.data() + r.outN;
    if (dsp)
        dsp->process(&in, &out, m);
    else
        std::copy(in, in + m, out);
    if (r.ratio > 1)
        lowpass(out, m);   // capture plus rapide : filtrer avant la descente vers le taux du projet
    r.outN += m;

    // Descente au taux du projet : exactement n échantillons.
    if (r.outN < (int) std::ceil(n * r.ratio) + 3)
    {
        std::fill(x, x + n, 0.f);   // ne devrait pas arriver grâce à l'amorce
        return;
    }
    const int back = r.down.process(r.ratio, r.out.data(), x, n, r.outN, 0);
    r.outN -= back;
    std::move(r.out.begin() + back, r.out.begin() + back + r.outN, r.out.begin());
}

juce::String ToneForgeProcessor::loadIr(int i, const juce::File& f)
{
    if (!f.existsAsFile())
        return "IR introuvable.";
    cabs[i].loadImpulseResponse(f, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::yes, 0,
                                juce::dsp::Convolution::Normalise::yes);
    irLoaded[i] = true;
    {
        const juce::ScopedLock fl(fileLock);
        irFiles[i] = f;
    }
    ++loadSerial;
    return {};
}

void ToneForgeProcessor::unloadIr(int i)
{
    {
        const juce::ScopedLock fl(fileLock);
        if (irFiles[i] == juce::File())
            return;
        irFiles[i] = juce::File();
    }
    irLoaded[i] = false;
    ++loadSerial;
}

void ToneForgeProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    juce::ValueTree s;
    {
        const juce::ScopedLock sl(stateLock);
        s = apvts.copyState();
    }
    {
        const juce::ScopedLock fl(fileLock);
        for (int i = 0; i < kNams; ++i)
            s.setProperty(kNamProp[i], namFiles[i].getFullPathName(), nullptr);
        for (int i = 0; i < kIrs; ++i)
            s.setProperty(kIrProp[i], irFiles[i].getFullPathName(), nullptr);
    }
    if (auto xml = s.createXml())
        copyXmlToBinary(*xml, dest);
}

// Numéro d'emplacement d'un nom de pédale (« pedal », « pedal2 »...), -1 sinon
static int pedalSlotOfName(const juce::String& nm)
{
    for (int i = 0; i < ToneForgeProcessor::kPedals; ++i)
        if (nm == ToneForgeProcessor::pedalId(i))
            return i;
    return -1;
}

// Chaîne d'avant le routage A/B : gate, unités, « amp » = Ampli A + Baffle A en parallèle d'Ampli B + Baffle B, puis EQ
static juce::StringArray migrateChain(const juce::StringArray& old)
{
    juce::StringArray r { "gate1" };
    for (auto nm : old)
    {
        nm = nm.trim();
        if (nm == "amp")
            r.addArray(juce::StringArray { "split", "nam1", "ir1", "|", "nam2", "ir2", "mix", "eq1" });
        else if (const int s = pedalSlotOfName(nm); s >= 0)
            r.add("nam" + juce::String(s + 3));
        else
            r.add(nm);
    }
    return r;
}

juce::StringArray ToneForgeProcessor::defaultRoute()
{
    return migrateChain({ "comp", "pedal", "amp", "chorus", "delay", "reverb" });
}

void ToneForgeProcessor::setStateInformation(const void* data, int size)
{
    auto xml = getXmlFromBinary(data, size);
    if (!xml)
        return;
    auto s = juce::ValueTree::fromXml(*xml);
    {
        const juce::ScopedLock sl(stateLock);
        apvts.replaceState(s);
    }
    lastMacro = apvts.getRawParameterValue("macro")->load();   // les cibles sauvées priment : pas de ré-application
    // Ancien état (sans routage A/B, voire sans « chain ») : converti
    auto route = juce::StringArray::fromTokens(s["chain"].toString(), ",", {});
    const bool legacy = !route.contains("split");
    if (legacy)
        route = s.hasProperty("chain") ? migrateChain(route) : defaultRoute();
    setRoute(route);
    for (int i = 0; i < kNams; ++i)
    {
        const juce::File f(s.getProperty(kNamProp[i]).toString());
        if (f.existsAsFile())
            loadNam(i, f);
        else if (i > 0)
            unloadNam(i);   // état sans cette capture : retirer celle en cours (l'ampli A reste, comme avant)
    }
    for (int i = 0; i < kIrs; ++i)
    {
        const juce::File f(s.getProperty(kIrProp[i]).toString());
        if (f.existsAsFile())
            loadIr(i, f);
        else if (i > 0)
            unloadIr(i);
    }
    // Ancien Ampli B allumé sans capture : il ne jouait pas (la chaîne B passerait maintenant le son direct)
    if (legacy && loadedFile(3) == juce::File())
        if (auto* p = apvts.getParameter("ampb_on"))
            p->setValueNotifyingHost(0.f);
}

void ToneForgeProcessor::unloadNam(int i)
{
    if (const juce::ScopedLock fl(fileLock); namFiles[i] == juce::File())
        return;
    std::unique_ptr<nam::DSP> old;   // détruit hors du verrou
    {
        auto& p = nams[i];
        const juce::SpinLock::ScopedLockType sl(p.lock);
        std::swap(p.model, old);
        p.captureRate = 0;
        p.loaded = false;
    }
    {
        const juce::ScopedLock fl(fileLock);
        namFiles[i] = juce::File();
    }
    ++loadSerial;
}

void ToneForgeProcessor::setRoute(const juce::StringArray& names)
{
    publishChain(names);
    const juce::ScopedLock sl(stateLock);
    apvts.state.setProperty("chain", getRoute().joinIntoString(","), nullptr);
}

// Ancienne interface : pédales 1-4 et effets (1er exemplaire) mobiles, « amp » = tout le reste sauf le gate 1
static bool legacyMobile(int c) { return (c >= kNamUnit + 2 && c < kNamUnit + 6) || (c >= 0 && c < (int) fxDefs().size()); }

juce::StringArray ToneForgeProcessor::getChain() const
{
    juce::StringArray r;
    for (auto& nm : getRoute())
    {
        const int c = unitCode(nm);
        if (legacyMobile(c))
            r.add(c >= kNamUnit ? pedalId(c - kNamUnit - 2) : nm);
        else if (c != kGateUnit && !r.contains("amp"))
            r.add("amp");
    }
    return r;
}

void ToneForgeProcessor::setChain(const juce::StringArray& names)
{
    const auto cur = getRoute();
    juce::StringArray core, r;
    for (auto& nm : cur)
        if (const int c = unitCode(nm); !legacyMobile(c) && c != kGateUnit)
            core.add(nm);
    if (cur.contains("gate1"))
        r.add("gate1");
    for (auto nm : names)
    {
        nm = nm.trim();
        if (nm == "amp")
            r.addArray(core);
        else if (const int s = pedalSlotOfName(nm); s >= 0)
            r.add("nam" + juce::String(s + 3));
        else
            r.add(nm);
    }
    setRoute(r);
}

// Corrige le routage (noms connus, sans doublon, repères split | mix dans l'ordre) et le passe au thread audio.
void ToneForgeProcessor::publishChain(const juce::StringArray& names)
{
    juce::StringArray sec[4];   // avant, A, B, après
    juce::Array<int> seen;
    int s = 0;
    for (auto nm : names)
    {
        nm = nm.trim();
        const int c = unitCode(nm);
        if (c < 0 || seen.contains(c))
            continue;
        seen.add(c);
        if (c < kSplit)
            sec[s].add(nm);
        else if (c - kSplit == s)   // repère hors d'ordre : ignoré
            s = c - kSplit + 1;
    }
    juce::StringArray all(sec[0]);
    all.add("split");
    all.addArray(sec[1]);
    all.add("|");
    all.addArray(sec[2]);
    all.add("mix");
    all.addArray(sec[3]);
    Chain ch;
    for (auto& nm : all)
        ch.u[ch.n++] = unitCode(nm);   // au plus 8 effets + 16 blocs + 3 repères < 40
    {
        const juce::ScopedLock sl(chainNamesLock);
        chainNames = all;
    }
    const juce::SpinLock::ScopedLockType sl(chainLock);
    chainPending = ch;
    chainDirty = true;
}

float ToneForgeProcessor::namLatency(int i) const
{
    return nams[i].loaded && namPar[i][0]->load() > 0.5f ? nams[i].latency.load() : 0.f;
}

float ToneForgeProcessor::rowLatency(int from, int to) const
{
    float l = 0.f;
    for (int j = from; j < to; ++j)
        if (const int c = chainCur.u[j]; c >= kNamUnit && c < kNamUnit + kNams)
            l += namLatency(c - kNamUnit);
    return l;
}

void ToneForgeProcessor::runUnit(int u, float* l, float* r, int n)
{
    if (u >= kSplit)
        return;
    if (u >= kIrUnit)   // gate, EQ, IR : bloc mono ou stéréo
    {
        float* chans[2] { l, r };
        juce::dsp::AudioBlock<float> blk(chans, r ? 2u : 1u, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx(blk);
        if (u >= kGateUnit)
        {
            const int i = u - kGateUnit;
            if (gatePar[i][0]->load() > 0.5f)
            {
                gates[i].setThreshold(gatePar[i][1]->load());
                gates[i].process(ctx);
            }
        }
        else if (u >= kEqUnit)
        {
            updateEq(u - kEqUnit);
            eqs[u - kEqUnit].process(ctx);
        }
        else if (irLoaded[u - kIrUnit] && irOn[u - kIrUnit]->load() > 0.5f)
            cabs[u - kIrUnit].process(ctx);
        return;
    }
    if (u >= kNamUnit)
    {
        runNamUnit(u - kNamUnit, l, n);
        if (r)   // NAM est mono : après le mélangeur, les deux canaux reçoivent sa sortie
            std::copy(l, l + n, r);
        return;
    }
    if (r)
        return runFx((size_t) u, l, r, n);
    chunks(n, scratch.size(), [&](int o, int c)   // mono : canal droit fictif
    {
        std::copy(l + o, l + o + c, scratch.data());
        runFx((size_t) u, l + o, scratch.data(), c);
    });
}

// Bloc NAM (ampli ou pédale) : gains, Tone (plateau d'aigus après la capture), Blend (son direct retardé comme la capture)
void ToneForgeProcessor::runNamUnit(int i, float* x, int n)
{
    auto& s = nams[i];
    auto& px = namX[i];
    if (namPar[i][0]->load() < 0.5f || !s.loaded)
        return;
    const float gIn = juce::Decibels::decibelsToGain(namPar[i][1]->load()),
                gOut = juce::Decibels::decibelsToGain(namPar[i][2]->load()),
                tone = namPar[i][3]->load(), mix = namPar[i][4]->load(), lat = s.latency.load();
    if (tone != px.tone)   // plateau d'aigus RBJ à 1,2 kHz, ±12 dB (0,5 = neutre)
    {
        px.tone = tone;
        const double A = std::pow(10.0, (tone - 0.5) * 24.0 / 40.0), w = juce::MathConstants<double>::twoPi * 1200.0 / sr,
                     cw = std::cos(w), al = std::sin(w) / 2.0 * std::sqrt(2.0), sa = 2.0 * std::sqrt(A) * al;
        const double a0 = (A + 1) - (A - 1) * cw + sa;
        px.b0 = (float) (A * ((A + 1) + (A - 1) * cw + sa) / a0);
        px.b1 = (float) (-2 * A * ((A - 1) + (A + 1) * cw) / a0);
        px.b2 = (float) (A * ((A + 1) + (A - 1) * cw - sa) / a0);
        px.a1 = (float) (2 * ((A - 1) - (A + 1) * cw) / a0);
        px.a2 = (float) (((A + 1) - (A - 1) * cw - sa) / a0);
    }
    chunks(n, dryBuf.size(), [&](int o, int c)
    {
        float* p = x + o;
        for (int k = 0; k < c; ++k)   // son direct, retardé comme la capture (sinon filtrage en peigne au Blend)
        {
            px.dry.pushSample(0, p[k]);
            dryBuf[(size_t) k] = px.dry.popSample(0, lat);
        }
        runNam(s, p, c, gIn, gOut);
        for (int k = 0; k < c; ++k)
        {
            const float in = p[k], y = px.b0 * in + px.z1;   // forme transposée II
            px.z1 = px.b1 * in - px.a1 * y + px.z2;
            px.z2 = px.b2 * in - px.a2 * y;
            p[k] = dryBuf[(size_t) k] + mix * (y - dryBuf[(size_t) k]);
        }
    });
}

juce::AudioProcessorEditor* ToneForgeProcessor::createEditor()
{
   #if JUCE_IOS
    return new PhoneEditor(*this);
   #else
    if (juce::SystemStats::getEnvironmentVariable("TF_PHONE", {}) == "1")   // essai de l'interface iPhone sur PC
        return new PhoneEditor(*this);
    return new ToneForgeEditor(*this);
   #endif
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ToneForgeProcessor(); }

void ToneForgeProcessor::runLooper(juce::AudioBuffer<float>& buffer, int n)
{
    const int max = loopBuf.getNumSamples(), nch = buffer.getNumChannels();
    if (const int c = loopCmd.exchange(-1); c >= 0 && max > 0)
    {
        const int st = loopState;
        if (c == loopClear)
        {
            loopState = loopEmpty;
            loopLen = loopPos = 0;
        }
        else if (c == loopRec && st == loopEmpty)
        {
            loopState = loopRec;
            loopLen = loopPos = 0;
        }
        else if (c == loopPlay && st != loopEmpty)
        {
            if (st == loopRec)
                loopLen = loopPos.load();
            if (st == loopRec || st == loopStop)
                loopPos = 0;
            loopState = loopLen > 0 ? loopPlay : loopEmpty;
        }
        else if ((c == loopDub && st == loopPlay) || (c == loopStop && st != loopEmpty))
        {
            if (st == loopRec)   // arrêt pendant la prise : la boucle garde ce qui est enregistré
                loopLen = loopPos.load();
            loopState = c;
        }
    }
    const int st = loopState;
    if (st == loopRec || st == loopPlay || st == loopDub)
    {
        int pos = loopPos, len = loopLen;
        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < nch; ++ch)
            {
                float* o = buffer.getWritePointer(ch);
                float* l = loopBuf.getWritePointer(juce::jmin(ch, 1));
                if (st == loopRec)
                    { if (ch < 2) l[pos] = o[i]; }
                else
                {
                    const float old = l[pos];
                    if (st == loopDub && ch < 2)
                        l[pos] = old + o[i];
                    o[i] += old;
                }
            }
            if (st == loopRec)
            {
                if (++pos >= max)   // 60 s atteintes : la boucle se referme et part en lecture
                {
                    loopLen = len = max;
                    pos = 0;
                    loopState = loopPlay;
                    loopPos = 0;
                    return;
                }
            }
            else if (++pos >= len)
                pos = 0;
        }
        loopPos = pos;
    }

    // Métronome : clic de 30 ms (temps 1 plus aigu), au tempo du delay
    if (!metroOn)
    {
        sinceBeat = 1e9;
        return;
    }
    const double spb = sr * 60.0 / juce::jlimit(30.f, 300.f, metroBpm.load());
    const int clickLen = (int) (sr * 0.03);
    for (int i = 0; i < n; ++i)
    {
        if (++sinceBeat >= spb)
        {
            const bool first = sinceBeat >= 1e8;   // départ : le premier clic est le temps 1
            sinceBeat = first ? 0.0 : sinceBeat - spb;
            clickBeat = first ? 0 : (clickBeat + 1) % 4;
            clickPos = 0;
        }
        if (clickPos < clickLen)
        {
            const double f = clickBeat == 0 ? 1500.0 : 1000.0, t = clickPos / sr;
            const float s = 0.35f * (float) (std::sin(juce::MathConstants<double>::twoPi * f * t) * std::exp(-t * 150.0));
            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer(ch)[i] += s;
            ++clickPos;
        }
    }
}
