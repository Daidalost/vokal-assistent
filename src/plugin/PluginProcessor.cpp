// Vokal Assistent - audio processor
#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace va;

namespace
{
class AnalysisJob : public juce::ThreadPoolJob
{
public:
    AnalysisJob (std::vector<float> audio, double sr, std::function<void (std::shared_ptr<AnalysisData>)> done)
        : juce::ThreadPoolJob ("Vokal Assistent analysis"), samples (std::move (audio)), fs (sr), onDone (std::move (done)) {}

    JobStatus runJob() override
    {
        auto result = std::make_shared<AnalysisData> (analyze (samples.data(), samples.size(), fs));
        if (! shouldExit())
            onDone (result);
        return jobHasFinished;
    }

private:
    std::vector<float> samples;
    double fs;
    std::function<void (std::shared_ptr<AnalysisData>)> onDone;
};
} // namespace

VokalAssistentProcessor::VokalAssistentProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "VokalAssistent", params::createLayout())
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bp = bandParams[(size_t) b];
        bp.on = apvts.getRawParameterValue (params::on (b));
        bp.freq = apvts.getRawParameterValue (params::freq (b));
        bp.gain = params::hasGain (b) ? apvts.getRawParameterValue (params::gain (b)) : nullptr;
        bp.q = params::hasQ (b) ? apvts.getRawParameterValue (params::q (b)) : nullptr;
    }
    hpfSlopeParam = apvts.getRawParameterValue (params::hpfSlope);
    dsOnParam = apvts.getRawParameterValue (params::dsOn);
    dsFreqParam = apvts.getRawParameterValue (params::dsFreq);
    dsThreshParam = apvts.getRawParameterValue (params::dsThresh);
    dsRangeParam = apvts.getRawParameterValue (params::dsRange);
    dsModeParam = apvts.getRawParameterValue (params::dsMode);
    dsListenParam = apvts.getRawParameterValue (params::dsListen);
    outGainParam = apvts.getRawParameterValue (params::outGain);
}

VokalAssistentProcessor::~VokalAssistentProcessor()
{
    pool.removeAllJobs (true, 10000);
    cancelPendingUpdate();
}

bool VokalAssistentProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void VokalAssistentProcessor::prepareToPlay (double sampleRate, int)
{
    if (recording.load() && sampleRate != recordSampleRate)
        cancelAnalysis();

    currentSampleRate = sampleRate;
    const int chans = juce::jmax (1, getTotalNumOutputChannels());
    eqChannels.assign ((size_t) chans, {});
    for (auto& c : eqChannels)
        c.prepare (sampleRate);
    deEsser.prepare (sampleRate, chans);
    firstBlock = true;
    outGainSmoothed.reset (sampleRate, 0.05);
    outGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain ((double) outGainParam->load()));
}

EqSettings VokalAssistentProcessor::readEqSettings() const
{
    EqSettings s = EqSettings::defaults();
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& band = s.bands[(size_t) b];
        const auto& bp = bandParams[(size_t) b];
        band.enabled = bp.on->load() > 0.5f;
        band.freq = bp.freq->load();
        band.gainDb = bp.gain != nullptr ? (double) bp.gain->load() : 0.0;
        band.q = bp.q != nullptr ? (double) bp.q->load() : 0.707;
    }
    s.hpfSteep = hpfSlopeParam->load() > 0.5f;
    return s;
}

DeEssSettings VokalAssistentProcessor::readDeEssSettings() const
{
    DeEssSettings d;
    d.enabled = dsOnParam->load() > 0.5f;
    d.freq = dsFreqParam->load();
    d.thresholdDb = dsThreshParam->load();
    d.rangeDb = dsRangeParam->load();
    d.focused = dsModeParam->load() > 0.5f;
    d.listen = dsListenParam->load() > 0.5f;
    return d;
}

// Moves the running EQ settings towards the parameter values (zipper-free automation).
void VokalAssistentProcessor::smoothTowards (const EqSettings& target, double alpha)
{
    currentEq.hpfSteep = target.hpfSteep;
    for (size_t b = 0; b < (size_t) kNumBands; ++b)
    {
        auto& c = currentEq.bands[b];
        const auto& t = target.bands[b];
        c.enabled = t.enabled;
        c.freq *= std::pow (t.freq / c.freq, alpha);
        c.q *= std::pow (t.q / c.q, alpha);
        c.gainDb += alpha * (t.gainDb - c.gainDb);
        if (std::abs (std::log (t.freq / c.freq)) < 1.0e-4) c.freq = t.freq;
        if (std::abs (std::log (t.q / c.q)) < 1.0e-4) c.q = t.q;
        if (std::abs (t.gainDb - c.gainDb) < 1.0e-3) c.gainDb = t.gainDb;
    }
}

static bool sameSettings (const EqSettings& a, const EqSettings& b)
{
    if (a.hpfSteep != b.hpfSteep) return false;
    for (size_t i = 0; i < (size_t) kNumBands; ++i)
    {
        const auto &x = a.bands[i], &y = b.bands[i];
        if (x.enabled != y.enabled || x.freq != y.freq || x.gainDb != y.gainDb || x.q != y.q) return false;
    }
    return true;
}

template <typename Sample>
void VokalAssistentProcessor::process (juce::AudioBuffer<Sample>& buffer)
{
    juce::ScopedNoDenormals noDenormals;
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    const int n = buffer.getNumSamples();
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, n);
    const int chans = juce::jmin (numOut, (int) eqChannels.size(), buffer.getNumChannels());
    if (chans == 0 || n == 0)
        return;

    // ---- record the unprocessed input for analysis
    if (recording.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType lock (recordLock);
        if (lock.isLocked() && ! recordBuffer.empty())
        {
            double peak = 0.0;
            for (int ch = 0; ch < chans; ++ch)
                for (int i = 0; i < n; ++i)
                    peak = std::max (peak, (double) std::abs (buffer.getSample (ch, i)));
            // Skip digital silence (transport stopped) so it does not fill the buffer.
            if (peak > 3.0e-5)
            {
                size_t pos = recordPos.load();
                const size_t todo = std::min ((size_t) n, recordBuffer.size() - pos);
                const float scale = 1.0f / (float) chans;
                for (size_t i = 0; i < todo; ++i)
                {
                    float m = 0.0f;
                    for (int ch = 0; ch < chans; ++ch)
                        m += (float) buffer.getSample (ch, (int) i);
                    recordBuffer[pos + i] = m * scale;
                }
                recordPos.store (pos + todo);
            }
        }
    }

    // ---- EQ (settings move towards the parameters every 32 samples)
    const EqSettings target = readEqSettings();
    if (firstBlock)
    {
        currentEq = target;
        for (auto& c : eqChannels) c.setSettings (currentEq);
        firstBlock = false;
    }
    constexpr int chunk = 32;
    const double alpha = 1.0 - std::exp (-(double) chunk / (0.03 * currentSampleRate));
    for (int start = 0; start < n; start += chunk)
    {
        const int len = juce::jmin (chunk, n - start);
        if (! sameSettings (currentEq, target))
        {
            smoothTowards (target, alpha);
            for (auto& c : eqChannels) c.setSettings (currentEq);
        }
        for (int ch = 0; ch < chans; ++ch)
        {
            auto* d = buffer.getWritePointer (ch, start);
            auto& eq = eqChannels[(size_t) ch];
            for (int i = 0; i < len; ++i)
                d[i] = (Sample) eq.process ((double) d[i]);
        }
    }

    // ---- de-esser
    deEsser.setSettings (readDeEssSettings());
    deEsser.process (buffer.getArrayOfWritePointers(), chans, n);
    deEssReduction.store ((float) deEsser.getCurrentReductionDb());

    // ---- output gain
    outGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain ((double) outGainParam->load()));
    for (int i = 0; i < n; ++i)
    {
        const double g = outGainSmoothed.getNextValue();
        for (int ch = 0; ch < chans; ++ch)
            buffer.getWritePointer (ch)[i] = (Sample) (buffer.getSample (ch, i) * g);
    }

    // ---- spectrum feed (mono)
    const auto scope = spectrumFifo.write (n);
    auto feed = [&] (int start, int size, int offset)
    {
        for (int i = 0; i < size; ++i)
        {
            float m = 0.0f;
            for (int ch = 0; ch < chans; ++ch) m += (float) buffer.getSample (ch, offset + i);
            spectrumBuffer[(size_t) (start + i)] = m / (float) chans;
        }
    };
    if (scope.blockSize1 > 0) feed (scope.startIndex1, scope.blockSize1, 0);
    if (scope.blockSize2 > 0) feed (scope.startIndex2, scope.blockSize2, scope.blockSize1);
}

void VokalAssistentProcessor::processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) { process (b); }
void VokalAssistentProcessor::processBlock (juce::AudioBuffer<double>& b, juce::MidiBuffer&) { process (b); }

// ------------------------------------------------------------------ analysis
void VokalAssistentProcessor::startAnalysis()
{
    if (analysisState.load() != AnalysisState::Idle)
        return;
    std::vector<float> fresh;
    try { fresh.assign ((size_t) (maxRecordSeconds * currentSampleRate), 0.0f); }
    catch (...) { lastError = "Not enough memory to record for analysis."; sendChangeMessage(); return; }
    {
        const juce::SpinLock::ScopedLockType lock (recordLock);
        recordBuffer.swap (fresh);
        recordPos.store (0);
        recordSampleRate = currentSampleRate;
    }
    lastError.clear();
    analysisState.store (AnalysisState::Recording);
    recording.store (true, std::memory_order_release);
    sendChangeMessage();
}

void VokalAssistentProcessor::cancelAnalysis()
{
    recording.store (false);
    {
        const juce::SpinLock::ScopedLockType lock (recordLock);
        std::vector<float>().swap (recordBuffer);
        recordPos.store (0);
    }
    analysisState.store (AnalysisState::Idle);
    sendChangeMessage();
}

void VokalAssistentProcessor::stopAnalysis()
{
    if (analysisState.load() != AnalysisState::Recording)
        return;
    recording.store (false);
    std::vector<float> audio;
    {
        const juce::SpinLock::ScopedLockType lock (recordLock);
        audio.swap (recordBuffer);
        audio.resize (recordPos.load());
        recordPos.store (0);
    }
    analysisState.store (AnalysisState::Analysing);
    sendChangeMessage();

    pool.addJob (new AnalysisJob (std::move (audio), recordSampleRate,
                                  [this] (std::shared_ptr<AnalysisData> r)
                                  {
                                      {
                                          const std::lock_guard<std::mutex> l (analysisMutex);
                                          pendingResult = std::move (r);
                                      }
                                      triggerAsyncUpdate();
                                  }),
                 true);
}

void VokalAssistentProcessor::handleAsyncUpdate()
{
    std::shared_ptr<AnalysisData> r;
    {
        const std::lock_guard<std::mutex> l (analysisMutex);
        r = std::move (pendingResult);
        if (r && r->valid)
            analysis = r;
    }
    if (r && ! r->valid)
        lastError = r->error;
    analysisState.store (AnalysisState::Idle);
    sendChangeMessage();
}

double VokalAssistentProcessor::getRecordedSeconds() const noexcept
{
    return recordSampleRate > 0.0 ? (double) recordPos.load() / recordSampleRate : 0.0;
}

bool VokalAssistentProcessor::recordingIsFull() const noexcept
{
    return recording.load() && (double) recordPos.load() >= maxRecordSeconds * recordSampleRate - 1.0;
}

std::shared_ptr<const AnalysisData> VokalAssistentProcessor::getAnalysis() const
{
    const std::lock_guard<std::mutex> l (analysisMutex);
    return analysis;
}

Suggestion VokalAssistentProcessor::getSuggestion() const
{
    const auto a = getAnalysis();
    if (a == nullptr)
        return {};
    const int p = juce::roundToInt (apvts.getRawParameterValue (params::profile)->load());
    const double amount = apvts.getRawParameterValue (params::amount)->load() / 100.0;
    return suggest (*a, (ProfileChoice) p, amount);
}

void VokalAssistentProcessor::applySuggestion()
{
    const auto s = getSuggestion();
    if (! s.valid)
        return;
    auto set = [this] (const juce::String& id, float value)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
    };
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& band = s.eq.bands[(size_t) b];
        set (params::on (b), band.enabled ? 1.0f : 0.0f);
        set (params::freq (b), (float) band.freq);
        if (params::hasGain (b)) set (params::gain (b), (float) band.gainDb);
        if (params::hasQ (b)) set (params::q (b), (float) band.q);
    }
    set (params::hpfSlope, s.eq.hpfSteep ? 1.0f : 0.0f);
    set (params::dsOn, s.deess.enabled ? 1.0f : 0.0f);
    set (params::dsFreq, (float) s.deess.freq);
    set (params::dsThresh, (float) s.deess.thresholdDb);
    set (params::dsRange, (float) s.deess.rangeDb);
    set (params::dsMode, s.deess.focused ? 1.0f : 0.0f);
    set (params::dsListen, 0.0f);
}

// ------------------------------------------------------------------ state
void VokalAssistentProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName ("Analysis"), nullptr);
    if (const auto a = getAnalysis())
    {
        juce::ValueTree t ("Analysis");
        t.setProperty ("data", juce::String (serialise (*a)), nullptr);
        state.appendChild (t, nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VokalAssistentProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;
    auto tree = juce::ValueTree::fromXml (*xml);
    const auto t = tree.getChildWithName ("Analysis");
    std::shared_ptr<AnalysisData> loaded;
    if (t.isValid())
    {
        auto d = std::make_shared<AnalysisData>();
        if (deserialise (t["data"].toString().toStdString(), *d) && d->valid)
            loaded = d;
        tree.removeChild (t, nullptr);
    }
    apvts.replaceState (tree);
    {
        const std::lock_guard<std::mutex> l (analysisMutex);
        analysis = loaded;
    }
    sendChangeMessage();
}

juce::AudioProcessorEditor* VokalAssistentProcessor::createEditor() { return new VokalAssistentEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VokalAssistentProcessor(); }
