#include "PluginProcessor.h"
#include "PluginEditor.h"
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

ToneForgeProcessor::ToneForgeProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", layout())
{
    for (auto& d : fxDefs())
    {
        fx.emplace_back(d.make(nullptr));
        const juce::String key(d.key);
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
    for (int i = 0; i < kPedals; ++i)
    {
        int j = 0;
        for (auto* suf : { "_on", "_in", "_out", "_tone", "_mix" })
            pedPar[i][j++] = apvts.getRawParameterValue(pedalId(i) + suf);
    }
    publishChain(defaultChain());
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
            spare[k].reset(defs[k].make(nullptr));   // détruit aussi l'ancienne instance usée
            spareReady[k].store(true, std::memory_order_release);
        }
    // Latence déclarée à l'hôte : pédales de la chaîne + ampli (le plus retardé de A et B quand B joue, cf. alignement)
    const auto lat = [](const NamSlot& s) { return s.loaded ? s.latency.load() : 0.f; };
    const bool bOn = apvts.getRawParameterValue("ampb_on")->load() > 0.5f && ampB.loaded;
    float pedLat = 0.f;
    const auto chain = getChain();
    for (int i = 0; i < kPedals; ++i)
        if (chain.contains(pedalId(i)) && pedPar[i][0]->load() > 0.5f)
            pedLat += lat(pedals[i]);
    const int latency = juce::roundToInt(pedLat + (bOn ? juce::jmax(lat(amp), lat(ampB)) : lat(amp)));
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
    for (auto* s : { &pedals[0], &pedals[1], &pedals[2], &pedals[3], &amp, &ampB })
    {
        const juce::SpinLock::ScopedLockType sl(s->lock);
        s->in.assign((size_t) samplesPerBlock, 0.f);
        s->out.assign((size_t) samplesPerBlock, 0.f);
        if (s->model)
            configureModel(*s->model, *s);
    }
    juce::dsp::ProcessSpec mono { sr, (juce::uint32) samplesPerBlock, 1 };
    gate.prepare(mono);
    for (auto& px : pedalX)
    {
        px.dry.prepare(mono);
        px.z1 = px.z2 = 0.f;
    }
    gate.setRatio(10.f);
    gate.setAttack(1.f);
    gate.setRelease(80.f);
    cab.prepare(mono);
    cabB.prepare(mono);
    alignA.prepare(mono);
    alignB.prepare(mono);
    eq.prepare(mono);
    lastEq[0] = 99.f;
    updateEq();
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

void ToneForgeProcessor::updateEq()
{
    const float b = apvts.getRawParameterValue("bass")->load(), m = apvts.getRawParameterValue("mid")->load(),
                t = apvts.getRawParameterValue("treble")->load();
    if (b == lastEq[0] && m == lastEq[1] && t == lastEq[2])
        return;   // recalcul seulement si un réglage a bougé
    lastEq[0] = b; lastEq[1] = m; lastEq[2] = t;
    auto db = [](float v) { return juce::Decibels::decibelsToGain(v); };
    using C = juce::dsp::IIR::Coefficients<float>;
    *eq.get<0>().state = *C::makeLowShelf(sr, 120.f, 0.7f, db(b));
    *eq.get<1>().state = *C::makePeakFilter(sr, 800.f, 0.8f, db(m));
    *eq.get<2>().state = *C::makeHighShelf(sr, 3500.f, 0.7f, db(t));
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
    juce::dsp::AudioBlock<float> monoBlock(&x, 1, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> monoCtx(monoBlock);

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
    if (v("gate_on") > 0.5f)
    {
        gate.setThreshold(v("gate"));
        gate.process(monoCtx);
    }
    int ci = 0;   // unités avant l'ampli, en mono
    for (; ci < chainCur.n && chainCur.u[ci] != kAmpUnit; ++ci)
        runUnit(chainCur.u[ci], x, nullptr, n);
    const float gIn = juce::Decibels::decibelsToGain(v("input"));
    const bool cabOn = irLoaded && v("ir_on") > 0.5f;
    const bool bOn = v("ampb_on") > 0.5f && ampB.loaded;
    if (bOn && !bActive)
    {
        cabB.reset();   // pas de reste de l'ancienne queue de convolution au rallumage
        alignA.reset();
        alignB.reset();
    }
    bActive = bOn;
    if (bOn)
    {
        // Amplis A et B en parallèle (modèle : ChainSlot Amp A/B + Cab A/B d'AmpliTube), fondu linéaire
        const float mix = v("ab_mix"), gbIn = juce::Decibels::decibelsToGain(v("ampb_in")),
                    gbOut = juce::Decibels::decibelsToGain(v("ampb_out"));
        const float latA = amp.loaded ? amp.latency.load() : 0.f, latB = ampB.latency.load();
        float shownMix = mix;
        chunks(n, bufB.size(), [&](int o, int c)
        {
            float* a = x + o;
            float* b = bufB.data();
            std::copy(a, a + c, b);
            juce::FloatVectorOperations::multiply(a, gIn, c);
            runNam(amp, a, c, 1.f, 1.f);
            runNam(ampB, b, c, gbIn, gbOut);
            const float dB = latA - latB;   // > 0 : B attend A, < 0 : A attend B
            auto& dl = dB > 0 ? alignB : alignA;
            float* p = dB > 0 ? b : a;
            if (std::abs(dB) > 0.01f)
                for (int i = 0; i < c; ++i)
                {
                    dl.pushSample(0, p[i]);
                    p[i] = dl.popSample(0, std::abs(dB));
                }
            juce::dsp::AudioBlock<float> ba(&a, 1, (size_t) c), bb(&b, 1, (size_t) c);
            if (cabOn)
                cab.process(juce::dsp::ProcessContextReplacing<float>(ba));
            if (irBLoaded)
                cabB.process(juce::dsp::ProcessContextReplacing<float>(bb));
            if (dynAmt > 0.f)
                for (int i = 0; i < c; ++i)
                {
                    const float t = dynMix[(size_t) juce::jmin(o + i, (int) dynMix.size() - 1)];
                    shownMix = mix + dynAmt * (t - mix);   // Attaque à fond : l'enveloppe remplace le mélange fixe
                    a[i] += shownMix * (b[i] - a[i]);
                }
            else
            {
                juce::FloatVectorOperations::multiply(a, 1.f - mix, c);
                juce::FloatVectorOperations::addWithMultiply(a, b, mix, c);
            }
        });
        dynPos.store(shownMix);
    }
    else
    {
        juce::FloatVectorOperations::multiply(x, gIn, n);
        runNam(amp, x, n, 1.f, 1.f);
        if (cabOn)
            cab.process(monoCtx);
    }
    updateEq();
    eq.process(monoCtx);

    for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom(ch, 0, buffer, 0, 0, n);

    // Unités après l'ampli, en stéréo (canal droit fictif si la sortie est mono)
    for (++ci; ci < chainCur.n; ++ci)
        runUnit(chainCur.u[ci], x, buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr, n);
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

juce::String ToneForgeProcessor::loadModel(const juce::File& f, bool isPedal, bool isB, int slot)
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
        auto& s = isPedal ? pedals[slot] : isB ? ampB : amp;
        const juce::SpinLock::ScopedLockType sl(s.lock);
        configureModel(*m, s);
        std::swap(s.model, m);
        s.loaded = true;
    }
    {
        const juce::ScopedLock fl(fileLock);
        (isPedal ? pedalFiles[slot] : isB ? modelBFile : modelFile) = f;
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

juce::String ToneForgeProcessor::loadIR(const juce::File& f, bool isB)
{
    if (!f.existsAsFile())
        return "IR introuvable.";
    (isB ? cabB : cab).loadImpulseResponse(f, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::yes, 0,
                                           juce::dsp::Convolution::Normalise::yes);
    (isB ? irBLoaded : irLoaded) = true;
    {
        const juce::ScopedLock fl(fileLock);
        (isB ? irBFile : irFile) = f;
    }
    ++loadSerial;
    return {};
}

void ToneForgeProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    juce::ValueTree s;
    {
        const juce::ScopedLock sl(stateLock);
        s = apvts.copyState();
    }
    s.setProperty("model", modelFile.getFullPathName(), nullptr);
    for (int i = 0; i < kPedals; ++i)
        s.setProperty(pedalId(i), loadedFile(pedalKind(i)).getFullPathName(), nullptr);
    s.setProperty("ir", irFile.getFullPathName(), nullptr);
    s.setProperty("modelB", modelBFile.getFullPathName(), nullptr);
    s.setProperty("irB", irBFile.getFullPathName(), nullptr);
    if (auto xml = s.createXml())
        copyXmlToBinary(*xml, dest);
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
    // Ancien état sans « chain » : ordre d'avant (compresseur, pédale, ampli, chorus, delay, reverb)
    publishChain(s.hasProperty("chain") ? juce::StringArray::fromTokens(s["chain"].toString(), ",", {}) : defaultChain());
    const juce::File m(s.getProperty("model").toString()), ir(s.getProperty("ir").toString());
    if (m.existsAsFile())
        loadModel(m);
    for (int i = 0; i < kPedals; ++i)
    {
        const juce::File pd(s.getProperty(pedalId(i)).toString());
        if (pd.existsAsFile())
            loadModel(pd, true, false, i);
        else
            unloadPedal(i);   // état sans cette pédale : retirer celle en cours
    }
    if (ir.existsAsFile())
        loadIR(ir);
    const juce::File mb(s.getProperty("modelB").toString()), ib(s.getProperty("irB").toString());
    if (mb.existsAsFile())
        loadModel(mb, false, true);
    else
        unloadB(true, false);   // état sans ampli B : retirer celui en cours
    if (ib.existsAsFile())
        loadIR(ib, true);
    else
        unloadB(false, true);
}

void ToneForgeProcessor::unloadPedal(int slot)
{
    if (loadedFile(pedalKind(slot)) == juce::File())
        return;
    std::unique_ptr<nam::DSP> old;   // détruit hors du verrou
    {
        auto& p = pedals[slot];
        const juce::SpinLock::ScopedLockType sl(p.lock);
        std::swap(p.model, old);
        p.captureRate = 0;
        p.loaded = false;
    }
    {
        const juce::ScopedLock fl(fileLock);
        pedalFiles[slot] = juce::File();
    }
    ++loadSerial;
}

void ToneForgeProcessor::setChain(const juce::StringArray& names)
{
    publishChain(names);
    const juce::ScopedLock sl(stateLock);
    apvts.state.setProperty("chain", getChain().joinIntoString(","), nullptr);
}

// Numéro d'emplacement d'un nom de pédale (« pedal », « pedal2 »...), -1 sinon
static int pedalSlotOfName(const juce::String& nm)
{
    for (int i = 0; i < ToneForgeProcessor::kPedals; ++i)
        if (nm == ToneForgeProcessor::pedalId(i))
            return i;
    return -1;
}

// Corrige l'ordre (noms connus, sans doublon, un seul « amp », pédales avant l'ampli) et le passe au thread audio.
void ToneForgeProcessor::publishChain(const juce::StringArray& names)
{
    const auto& defs = fxDefs();
    const auto fxIndex = [&defs](const juce::String& nm)
    {
        for (size_t k = 0; k < defs.size(); ++k)
            if (nm == defs[k].key)
                return (int) k;
        return -1;
    };
    juce::StringArray pre, post;
    bool afterAmp = false;
    for (auto nm : names)
    {
        nm = nm.trim();
        if (nm == "amp")
            afterAmp = true;
        else if ((pedalSlotOfName(nm) >= 0 || fxIndex(nm) >= 0) && !pre.contains(nm) && !post.contains(nm))
            (afterAmp && pedalSlotOfName(nm) < 0 ? post : pre).add(nm);
    }
    juce::StringArray all(pre);
    all.add("amp");
    all.addArray(post);
    Chain c;
    for (auto& nm : all)
    {
        const int ps = pedalSlotOfName(nm), k = fxIndex(nm);
        c.u[c.n++] = ps >= 0 ? 10 + ps : k >= 0 ? k : kAmpUnit;   // au plus 4 pédales + effets + ampli < 16
    }
    {
        const juce::ScopedLock sl(chainNamesLock);
        chainNames = all;
    }
    const juce::SpinLock::ScopedLockType sl(chainLock);
    chainPending = c;
    chainDirty = true;
}

void ToneForgeProcessor::runUnit(int u, float* l, float* r, int n)
{
    if (u >= 10)
        return runPedal(u - 10, l, n);
    if (r)
        return runFx((size_t) u, l, r, n);
    chunks(n, scratch.size(), [&](int o, int c)   // mono : canal droit fictif
    {
        std::copy(l + o, l + o + c, scratch.data());
        runFx((size_t) u, l + o, scratch.data(), c);
    });
}

void ToneForgeProcessor::runPedal(int i, float* x, int n)
{
    auto& s = pedals[i];
    auto& px = pedalX[i];
    if (pedPar[i][0]->load() < 0.5f || !s.loaded)
        return;
    const float gIn = juce::Decibels::decibelsToGain(pedPar[i][1]->load()),
                gOut = juce::Decibels::decibelsToGain(pedPar[i][2]->load()),
                tone = pedPar[i][3]->load(), mix = pedPar[i][4]->load(), lat = s.latency.load();
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

void ToneForgeProcessor::unloadB(bool model, bool ir)
{
    if (model && modelBFile != juce::File())
    {
        std::unique_ptr<nam::DSP> old;
        {
            const juce::SpinLock::ScopedLockType sl(ampB.lock);
            std::swap(ampB.model, old);
            ampB.captureRate = 0;
            ampB.loaded = false;
        }
        const juce::ScopedLock fl(fileLock);
        modelBFile = juce::File();
    }
    if (ir && irBFile != juce::File())
    {
        irBLoaded = false;
        const juce::ScopedLock fl(fileLock);
        irBFile = juce::File();
    }
    ++loadSerial;
}

juce::AudioProcessorEditor* ToneForgeProcessor::createEditor() { return new ToneForgeEditor(*this); }

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
