#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "NAM/get_dsp.h"

static juce::String u8p(const char* s) { return juce::String(juce::CharPointer_UTF8(s)); }

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
    startTimer(250);
}

void ToneForgeProcessor::timerCallback()
{
    const auto& defs = fxDefs();
    for (size_t k = 0; k < spare.size(); ++k)
        if (!spareReady[k].load(std::memory_order_acquire))
        {
            spare[k].reset(defs[k].make(nullptr));   // détruit aussi l'ancienne instance usée
            spareReady[k].store(true, std::memory_order_release);
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
    p.push_back(std::make_unique<juce::AudioParameterBool>("pedal_on", u8p("Pédale"), true));
    p.push_back(std::make_unique<P>("pedal_in", u8p("Pédale entrée (dB)"), R(-24.f, 24.f), 0.f));
    p.push_back(std::make_unique<P>("pedal_out", u8p("Pédale sortie (dB)"), R(-24.f, 24.f), 0.f));
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
    for (auto* s : { &pedal, &amp })
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
    peak(inPeak, buffer.getMagnitude(0, 0, n));

    // Guitare = mono : on traite le canal 0, puis on recopie vers les autres.
    float* x = buffer.getWritePointer(0);
    juce::dsp::AudioBlock<float> monoBlock(&x, 1, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> monoCtx(monoBlock);

    // Les tampons internes font maxBlock : un bloc plus long est traité par tranches.
    auto chunks = [n](size_t cap, auto&& f)
    {
        const int c = (int) cap;
        for (int o = 0; c > 0 && o < n; o += c)
            f(o, juce::jmin(c, n - o));
    };

    if (v("gate_on") > 0.5f)
    {
        gate.setThreshold(v("gate"));
        gate.process(monoCtx);
    }
    chunks(scratch.size(), [&](int o, int c)
    {
        std::copy(x + o, x + o + c, scratch.data());
        runFx(true, x + o, scratch.data(), c);   // compresseur avant l'ampli, en mono
    });
    if (v("pedal_on") > 0.5f)
        runNam(pedal, x, n, juce::Decibels::decibelsToGain(v("pedal_in")), juce::Decibels::decibelsToGain(v("pedal_out")));
    juce::FloatVectorOperations::multiply(x, juce::Decibels::decibelsToGain(v("input")), n);
    runNam(amp, x, n, 1.f, 1.f);

    if (irLoaded && v("ir_on") > 0.5f)
        cab.process(monoCtx);
    updateEq();
    eq.process(monoCtx);

    for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom(ch, 0, buffer, 0, 0, n);

    // Chorus, delay, reverb en stéréo (canal droit fictif si la sortie est mono)
    if (buffer.getNumChannels() > 1)
        runFx(false, x, buffer.getWritePointer(1), n);
    else
        chunks(scratch.size(), [&](int o, int c)
        {
            std::copy(x + o, x + o + c, scratch.data());
            runFx(false, x + o, scratch.data(), c);
        });
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom(ch, 0, buffer, ch % 2, 0, n);

    buffer.applyGain(juce::Decibels::decibelsToGain(v("output")));
    peak(outPeak, buffer.getMagnitude(0, 0, n));
}

// Capture NAM d'un emplacement ; les gains ne s'appliquent que si une capture est chargée.
void ToneForgeProcessor::runNam(NamSlot& s, float* x, int n, float gainIn, float gainOut)
{
    const juce::SpinLock::ScopedTryLockType sl(s.lock);
    if (!sl.isLocked() || !s.model)
        return;
    juce::FloatVectorOperations::multiply(x, gainIn, n);
    // Les tampons internes font maxBlock : un bloc plus long est traité par tranches.
    const int cap = (int) s.in.size();
    for (int o = 0; cap > 0 && o < n; o += cap)
    {
        const int c = juce::jmin(cap, n - o);
        if (s.rs.on)
            processResampled(s, x + o, c);
        else
        {
            std::copy(x + o, x + o + c, s.in.data());
            float* in = s.in.data();
            float* out = s.out.data();
            s.model->process(&in, &out, c);
            std::copy(out, out + c, x + o);
        }
    }
    juce::FloatVectorOperations::multiply(x, gainOut, n);
}

juce::String ToneForgeProcessor::loadModel(const juce::File& f, bool isPedal)
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
        auto& s = isPedal ? pedal : amp;
        const juce::SpinLock::ScopedLockType sl(s.lock);
        configureModel(*m, s);
        std::swap(s.model, m);
    }
    (isPedal ? pedalFile : modelFile) = f;
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
    r.outN = 12;   // amorce de silence : la sortie ne manque jamais d'échantillons
}

void ToneForgeProcessor::processResampled(NamSlot& s, float* x, int n)
{
    auto& r = s.rs;
    std::copy(x, x + n, r.in.data() + r.inN);
    r.inN += n;

    // Montée au taux de la capture : on garde 3 échantillons d'avance pour l'interpolateur.
    const int m = juce::jlimit(0, (int) r.mid.size(),
                               juce::jmin((int) ((r.inN - 3) * r.ratio), (int) r.out.size() - r.outN));
    const int used = r.up.process(1.0 / r.ratio, r.in.data(), r.mid.data(), m, r.inN, 0);
    r.inN -= used;
    std::move(r.in.begin() + used, r.in.begin() + used + r.inN, r.in.begin());

    float* in = r.mid.data();
    float* out = r.out.data() + r.outN;
    s.model->process(&in, &out, m);
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

juce::String ToneForgeProcessor::loadIR(const juce::File& f)
{
    if (!f.existsAsFile())
        return "IR introuvable.";
    cab.loadImpulseResponse(f, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::yes, 0,
                            juce::dsp::Convolution::Normalise::yes);
    irLoaded = true;
    irFile = f;
    return {};
}

void ToneForgeProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    auto s = apvts.copyState();
    s.setProperty("model", modelFile.getFullPathName(), nullptr);
    s.setProperty("pedal", pedalFile.getFullPathName(), nullptr);
    s.setProperty("ir", irFile.getFullPathName(), nullptr);
    if (auto xml = s.createXml())
        copyXmlToBinary(*xml, dest);
}

void ToneForgeProcessor::setStateInformation(const void* data, int size)
{
    auto xml = getXmlFromBinary(data, size);
    if (!xml)
        return;
    auto s = juce::ValueTree::fromXml(*xml);
    apvts.replaceState(s);
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
        }
        pedalFile = juce::File();
    }
    if (ir.existsAsFile())
        loadIR(ir);
}

juce::AudioProcessorEditor* ToneForgeProcessor::createEditor() { return new ToneForgeEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ToneForgeProcessor(); }
