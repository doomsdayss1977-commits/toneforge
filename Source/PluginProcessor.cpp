#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "NAM/get_dsp.h"

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
}

juce::AudioProcessorValueTreeState::ParameterLayout ToneForgeProcessor::layout()
{
    using P = juce::AudioParameterFloat;
    using R = juce::NormalisableRange<float>;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<P>("gate", "Gate (dB)", R(-100.f, -20.f), -70.f));
    p.push_back(std::make_unique<P>("input", "Entrée (dB)", R(-24.f, 24.f), 0.f));
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
    namIn.assign((size_t) samplesPerBlock, 0.f);
    namOut.assign((size_t) samplesPerBlock, 0.f);
    scratch.assign((size_t) samplesPerBlock, 0.f);
    for (auto& e : fx)
        e->setSampleRate((float) sampleRate);
    {
        const juce::SpinLock::ScopedLockType sl(modelLock);
        if (model)
            model->Reset(sr, maxBlock);
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
        if (defs[k].preAmp != preAmp || fxOn[k]->load() < 0.5f)
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

    // Guitare = mono : on traite le canal 0, puis on recopie vers les autres.
    float* x = buffer.getWritePointer(0);
    juce::dsp::AudioBlock<float> monoBlock(&x, 1, (size_t) n);
    juce::dsp::ProcessContextReplacing<float> monoCtx(monoBlock);

    gate.setThreshold(v("gate"));
    gate.process(monoCtx);
    if (n <= (int) scratch.size())
    {
        std::copy(x, x + n, scratch.data());
        runFx(true, x, scratch.data(), n);   // compresseur avant l'ampli, en mono
    }
    juce::FloatVectorOperations::multiply(x, juce::Decibels::decibelsToGain(v("input")), n);

    {
        const juce::SpinLock::ScopedTryLockType sl(modelLock);
        if (sl.isLocked() && model && n <= (int) namIn.size())
        {
            std::copy(x, x + n, namIn.data());
            float* in = namIn.data();
            float* out = namOut.data();
            model->process(&in, &out, n);
            std::copy(out, out + n, x);
        }
    }

    if (irLoaded)
        cab.process(monoCtx);
    updateEq();
    eq.process(monoCtx);

    for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom(ch, 0, buffer, 0, 0, n);

    // Chorus, delay, reverb en stéréo (canal droit fictif si la sortie est mono)
    float* r = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : scratch.data();
    if (buffer.getNumChannels() > 1 || n <= (int) scratch.size())
    {
        if (buffer.getNumChannels() == 1)
            std::copy(x, x + n, r);
        runFx(false, x, r, n);
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom(ch, 0, buffer, ch % 2, 0, n);

    buffer.applyGain(juce::Decibels::decibelsToGain(v("output")));
}

juce::String ToneForgeProcessor::loadModel(const juce::File& f)
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
    const double want = m->GetExpectedSampleRate();
    modelWarning = (want > 0 && std::abs(want - sr) > 1 && !m->SupportsArbitrarySampleRate())
        ? juce::String("Capture faite à ") + juce::String((int) want) + " Hz, projet à " + juce::String((int) sr)
              + " Hz : mettez le projet à " + juce::String((int) want) + " Hz."
        : juce::String();
    m->Reset(sr, maxBlock);
    {
        const juce::SpinLock::ScopedLockType sl(modelLock);
        std::swap(model, m);
    }
    modelFile = f;
    return {};   // l'ancien modèle est détruit ici, hors du verrou
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
    const juce::File m(s.getProperty("model").toString()), ir(s.getProperty("ir").toString());
    if (m.existsAsFile())
        loadModel(m);
    if (ir.existsAsFile())
        loadIR(ir);
}

juce::AudioProcessorEditor* ToneForgeProcessor::createEditor() { return new ToneForgeEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ToneForgeProcessor(); }
