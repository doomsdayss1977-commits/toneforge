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
    // Latence déclarée à l'hôte : pédale + ampli (le plus retardé de A et B quand B joue, cf. alignement)
    const auto lat = [](const NamSlot& s) { return s.loaded ? s.latency.load() : 0.f; };
    const bool bOn = apvts.getRawParameterValue("ampb_on")->load() > 0.5f && ampB.loaded;
    const int latency = juce::roundToInt((apvts.getRawParameterValue("pedal_on")->load() > 0.5f ? lat(pedal) : 0.f)
                                         + (bOn ? juce::jmax(lat(amp), lat(ampB)) : lat(amp)));
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
    if (loopBuf.getNumSamples() != (int) (sampleRate * 60.0))   // même taux : la boucle survit à un nouveau prepare
    {
        loopBuf.setSize(2, (int) (sampleRate * 60.0));
        loopBuf.clear();
        loopState = loopEmpty;
        loopLen = loopPos = 0;
    }
    for (auto* s : { &pedal, &amp, &ampB })
    {
        const juce::SpinLock::ScopedLockType sl(s->lock);
        s->in.assign((size_t) samplesPerBlock, 0.f);
        s->out.assign((size_t) samplesPerBlock, 0.f);
        if (s->model)
            configureModel(*s->model, *s);
    }
    juce::dsp::ProcessSpec mono { sr, (juce::uint32) samplesPerBlock, 1 };
    gate.prepare(mono);
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

void ToneForgeProcessor::runFx(bool preAmp, float* l, float* r, int n)
{
    const auto& defs = fxDefs();
    for (size_t k = 0; k < defs.size(); ++k)
    {
        const bool on = fxOn[k]->load() >= 0.5f;
        if (defs[k].preAmp != preAmp)
            continue;
        if (on && !lastOn[k] && spareReady[k].load(std::memory_order_acquire))
        {
            std::swap(fx[k], spare[k]);   // repart avec des tampons vides, sans allouer ici
            fx[k]->setSampleRate((float) sr);
            spareReady[k].store(false, std::memory_order_release);
        }
        lastOn[k] = on;
        if (!on)
            continue;
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

    if (v("gate_on") > 0.5f)
    {
        gate.setThreshold(v("gate"));
        gate.process(monoCtx);
    }
    chunks(n, scratch.size(), [&](int o, int c)
    {
        std::copy(x + o, x + o + c, scratch.data());
        runFx(true, x + o, scratch.data(), c);   // compresseur avant l'ampli, en mono
    });
    if (v("pedal_on") > 0.5f)
        runNam(pedal, x, n, juce::Decibels::decibelsToGain(v("pedal_in")), juce::Decibels::decibelsToGain(v("pedal_out")));
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
            juce::FloatVectorOperations::multiply(a, 1.f - mix, c);
            juce::FloatVectorOperations::addWithMultiply(a, b, mix, c);
        });
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

    // Chorus, delay, reverb en stéréo (canal droit fictif si la sortie est mono)
    if (buffer.getNumChannels() > 1)
        runFx(false, x, buffer.getWritePointer(1), n);
    else
        chunks(n, scratch.size(), [&](int o, int c)
        {
            std::copy(x + o, x + o + c, scratch.data());
            runFx(false, x + o, scratch.data(), c);
        });
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

juce::String ToneForgeProcessor::loadModel(const juce::File& f, bool isPedal, bool isB)
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
        auto& s = isPedal ? pedal : isB ? ampB : amp;
        const juce::SpinLock::ScopedLockType sl(s.lock);
        configureModel(*m, s);
        std::swap(s.model, m);
        s.loaded = true;
    }
    {
        const juce::ScopedLock fl(fileLock);
        (isPedal ? pedalFile : isB ? modelBFile : modelFile) = f;
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
    s.setProperty("pedal", pedalFile.getFullPathName(), nullptr);
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
    const juce::File m(s.getProperty("model").toString()), pd(s.getProperty("pedal").toString()),
        ir(s.getProperty("ir").toString());
    if (m.existsAsFile())
        loadModel(m);
    if (pd.existsAsFile())
        loadModel(pd, true);
    else if (pedalFile != juce::File())   // état sans pédale : retirer celle en cours
    {
        std::unique_ptr<nam::DSP> old;
        {
            const juce::SpinLock::ScopedLockType sl(pedal.lock);
            std::swap(pedal.model, old);
            pedal.captureRate = 0;
            pedal.loaded = false;
        }
        const juce::ScopedLock fl(fileLock);
        pedalFile = juce::File();
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
