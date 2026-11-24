// Vokal Assistent - EQ curve, live spectrum and analysis overlay
#include "EqDisplay.h"
#include "Theme.h"

using namespace va;

EqDisplay::EqDisplay (VokalAssistentProcessor& p) : proc (p)
{
    setRepaintsOnMouseActivity (false);
}

juce::Rectangle<float> EqDisplay::plotArea() const { return getLocalBounds().toFloat().reduced (34.0f, 10.0f).withTrimmedBottom (12.0f); }

float EqDisplay::xForFreq (double f) const
{
    const auto r = plotArea();
    return r.getX() + r.getWidth() * (float) (std::log (f / minHz) / std::log (maxHz / minHz));
}
double EqDisplay::freqForX (float x) const
{
    const auto r = plotArea();
    return minHz * std::pow (maxHz / minHz, (double) ((x - r.getX()) / r.getWidth()));
}
float EqDisplay::yForDb (double db) const
{
    const auto r = plotArea();
    return r.getCentreY() - (float) (db / dbRange) * r.getHeight() * 0.5f;
}
double EqDisplay::dbForY (float y) const
{
    const auto r = plotArea();
    return (double) ((r.getCentreY() - y) / (r.getHeight() * 0.5f)) * dbRange;
}

juce::Point<float> EqDisplay::handlePos (int band, const EqSettings& s) const
{
    const auto& b = s.bands[(size_t) band];
    const double db = band == kHpf ? 0.0 : b.gainDb;
    return { xForFreq (b.freq), yForDb (db) };
}

int EqDisplay::hitBand (juce::Point<float> p) const
{
    const auto s = proc.readEqSettings();
    int best = -1;
    float bestD = 12.0f;
    for (int b = 0; b < kNumBands; ++b)
    {
        const float d = handlePos (b, s).getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = b; }
    }
    return best;
}

void EqDisplay::setParam (const juce::String& id, float value)
{
    if (auto* p = proc.apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void EqDisplay::mouseMove (const juce::MouseEvent& e)
{
    const int h = hitBand (e.position);
    if (h != hoverBand) { hoverBand = h; repaint(); }
}
void EqDisplay::mouseExit (const juce::MouseEvent&) { hoverBand = -1; repaint(); }

void EqDisplay::mouseDown (const juce::MouseEvent& e)
{
    dragBand = hitBand (e.position);
    if (dragBand < 0) return;
    for (auto id : { params::freq (dragBand), params::gain (dragBand) })
        if (auto* p = proc.apvts.getParameter (id)) p->beginChangeGesture();
    if (auto* p = proc.apvts.getParameter (params::on (dragBand)))
        if (p->getValue() < 0.5f) { p->beginChangeGesture(); p->setValueNotifyingHost (1.0f); p->endChangeGesture(); }
}

void EqDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0) return;
    setParam (params::freq (dragBand), (float) freqForX (e.position.x));
    if (params::hasGain (dragBand))
        setParam (params::gain (dragBand), (float) juce::jlimit (-15.0, 15.0, dbForY (e.position.y)));
    repaint();
}

void EqDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragBand < 0) return;
    for (auto id : { params::freq (dragBand), params::gain (dragBand) })
        if (auto* p = proc.apvts.getParameter (id)) p->endChangeGesture();
    dragBand = -1;
}

void EqDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int b = hitBand (e.position);
    if (b < 0) return;
    if (auto* p = proc.apvts.getParameter (params::on (b)))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.0f : 1.0f);
        p->endChangeGesture();
    }
    repaint();
}

void EqDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int b = hitBand (e.position);
    if (b < 0 || ! params::hasQ (b)) return;
    if (auto* p = proc.apvts.getParameter (params::q (b)))
    {
        const float cur = proc.apvts.getRawParameterValue (params::q (b))->load();
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (0.3f, 10.0f, cur * (w.deltaY > 0 ? 1.12f : 1.0f / 1.12f))));
        p->endChangeGesture();
    }
    repaint();
}

void EqDisplay::pullSpectrum()
{
    auto& fifo = proc.spectrumFifo;
    const int ready = fifo.getNumReady();
    if (ready > 0)
    {
        const auto scope = fifo.read (ready);
        auto take = [&] (int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                ring[(size_t) ringPos] = proc.spectrumBuffer[(size_t) (start + i)];
                ringPos = (ringPos + 1) % fftSize;
            }
        };
        take (scope.startIndex1, scope.blockSize1);
        take (scope.startIndex2, scope.blockSize2);
    }

    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = ring[(size_t) ((ringPos + i) % fftSize)];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    const float norm = 2.0f / ((float) fftSize * 0.5f);   // Hann coherent gain 0.5
    for (size_t k = 0; k < spectrumDb.size(); ++k)
    {
        const float db = juce::Decibels::gainToDecibels (fftData[k] * norm, -120.0f);
        auto& s = spectrumDb[k];
        s = db > s ? 0.6f * db + 0.4f * s : 0.15f * db + 0.85f * s;   // fast rise, slow fall
    }
    repaint();
}

void EqDisplay::paint (juce::Graphics& g)
{
    using namespace theme;
    const auto area = plotArea();
    g.fillAll (bg());
    g.setColour (panel());
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);

    // ---- grid
    g.setFont (juce::FontOptions (11.0f));
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = xForFreq (f);
        g.setColour (grid());
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (textDim());
        const auto label = f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f);
        g.drawText (label, juce::Rectangle<float> (x - 20.0f, area.getBottom() + 1.0f, 40.0f, 12.0f), juce::Justification::centred);
    }
    for (double db = -15.0; db <= 15.0; db += 5.0)
    {
        const float y = yForDb (db);
        g.setColour (db == 0.0 ? gridStrong() : grid());
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        g.setColour (textDim());
        g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (2.0f, y - 6.0f, 28.0f, 12.0f), juce::Justification::centredRight);
    }

    const double fs = proc.getCurrentSampleRate();
    const auto eq = proc.readEqSettings();

    // ---- live spectrum (own scale: -90..0 dBFS over the full height)
    {
        juce::Path p;
        bool started = false;
        const double binHz = fs / fftSize;
        for (float x = area.getX(); x <= area.getRight(); x += 2.0f)
        {
            const double f = freqForX (x);
            const double pos = f / binHz;
            const size_t k = (size_t) juce::jlimit (1.0, (double) spectrumDb.size() - 2.0, pos);
            const float db = spectrumDb[k] + 3.0f * (float) std::log2 (juce::jmax (1.0, f / 1000.0));   // +3 dB/oct tilt, like a pink reference
            const float y = juce::jmap (juce::jlimit (-90.0f, 0.0f, db), -90.0f, 0.0f, area.getBottom(), area.getY());
            if (! started) { p.startNewSubPath (x, area.getBottom()); started = true; }
            p.lineTo (x, y);
        }
        p.lineTo (area.getRight(), area.getBottom());
        p.closeSubPath();
        g.setColour (spectrum().withAlpha (0.35f));
        g.fillPath (p);
    }

    // ---- analysis overlay
    if (showAnalysis && suggestion.valid && ! suggestion.measuredDb.empty())
    {
        auto an = proc.getAnalysis();
        if (an != nullptr)
        {
            const auto& hz = an->gridHz;
            // Shift so the target averages 0 dB over 200 Hz - 5 kHz.
            double sum = 0.0;
            int c = 0;
            for (size_t i = 0; i < hz.size(); ++i)
                if (hz[i] >= 200.0 && hz[i] <= 5000.0) { sum += suggestion.targetDb[i]; ++c; }
            const double off = c > 0 ? sum / c : 0.0;
            const double lo = std::max (60.0, suggestion.eq.bands[kHpf].freq * 1.2);
            const double hi = std::min (16000.0, 0.45 * an->sampleRate);

            auto curve = [&] (std::function<double (size_t)> value)
            {
                juce::Path p;
                bool started = false;
                for (size_t i = 0; i < hz.size(); ++i)
                {
                    if (hz[i] < lo || hz[i] > hi) continue;
                    const auto pt = juce::Point<float> (xForFreq (hz[i]), yForDb (juce::jlimit (-dbRange, dbRange, value (i) - off)));
                    if (! started) { p.startNewSubPath (pt); started = true; }
                    else p.lineTo (pt);
                }
                return p;
            };
            g.setColour (measured().withAlpha (0.9f));
            g.strokePath (curve ([&] (size_t i) { return suggestion.measuredDb[i]; }), juce::PathStrokeType (1.5f));
            g.setColour (after().withAlpha (0.9f));
            g.strokePath (curve ([&] (size_t i) { return suggestion.measuredDb[i] + eqResponseDb (eq, hz[i], an->sampleRate); }),
                          juce::PathStrokeType (1.5f));
            juce::Path target = curve ([&] (size_t i) { return suggestion.targetDb[i]; }), dashed;
            const float dashes[] = { 6.0f, 4.0f };
            juce::PathStrokeType (2.0f).createDashedStroke (dashed, target, dashes, 2);
            g.setColour (targetCol());
            g.fillPath (dashed);

            // Legend
            auto lgArea = area;
            auto lg = lgArea.removeFromTop (16.0f).removeFromRight (330.0f);
            auto item = [&] (juce::Colour col, const juce::String& text)
            {
                auto r = lg.removeFromLeft (110.0f);
                g.setColour (col);
                g.fillRect (r.removeFromLeft (14.0f).withSizeKeepingCentre (14.0f, 3.0f));
                g.setColour (textDim());
                g.drawText (text, r.withTrimmedLeft (4.0f), juce::Justification::centredLeft);
            };
            item (measured(), "Your voice");
            item (after(), "With EQ");
            item (targetCol(), "Target");
        }
    }

    // ---- EQ curve
    {
        juce::Path p;
        for (float x = area.getX(); x <= area.getRight(); x += 1.5f)
        {
            const double db = eqResponseDb (eq, freqForX (x), fs);
            const float y = yForDb (juce::jlimit (-dbRange * 1.2, dbRange * 1.2, db));
            if (x == area.getX()) p.startNewSubPath (x, y);
            else p.lineTo (x, y);
        }
        g.setColour (accent().withAlpha (0.12f));
        juce::Path fill (p);
        fill.lineTo (area.getRight(), yForDb (0.0));
        fill.lineTo (area.getX(), yForDb (0.0));
        fill.closeSubPath();
        g.fillPath (fill);
        g.setColour (accent());
        g.strokePath (p, juce::PathStrokeType (2.2f));
    }

    // ---- handles
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto pos = handlePos (b, eq);
        const bool on = eq.bands[(size_t) b].enabled;
        const float r = (b == hoverBand || b == dragBand) ? 9.0f : 7.5f;
        g.setColour (on ? bandColour (b) : textDim().withAlpha (0.5f));
        if (on) g.fillEllipse (pos.x - r, pos.y - r, 2 * r, 2 * r);
        else g.drawEllipse (pos.x - r, pos.y - r, 2 * r, 2 * r, 1.5f);
        g.setColour (on ? juce::Colours::black : textDim());
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (juce::String (b + 1), juce::Rectangle<float> (pos.x - r, pos.y - r, 2 * r, 2 * r), juce::Justification::centred);
    }

    if (hoverBand >= 0)
    {
        const auto& b = eq.bands[(size_t) hoverBand];
        juce::String info = params::bandName (hoverBand) + "  " + params::hzToText ((float) b.freq, 0);
        if (params::hasGain (hoverBand)) info << "  " << params::dbToText ((float) b.gainDb, 0);
        if (params::hasQ (hoverBand)) info << "  Q " << juce::String (b.q, 2);
        g.setColour (text());
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (info, area.withHeight (16.0f).translated (6.0f, 2.0f), juce::Justification::centredLeft);
    }
}
