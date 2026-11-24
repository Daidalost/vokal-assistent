// Vokal Assistent - offline vocal analysis and suggestion engine.
//
// Overview of the analysis (all offline, on the recorded mono signal):
//  1. Frames of ~85 ms (hop ~21 ms). Per frame: RMS, pitch (YIN on a 4x
//     decimated copy), the share of energy above 4 kHz and spectral centroid.
//  2. Frames are classified relative to the loudest parts of the take:
//     active (voice present), tonal (clear pitch, little HF noise) and
//     sibilant (energy dominated by 4-14 kHz).
//  3. Long-term average spectrum (LTAS) of the tonal frames, as 1/3 octave
//     band power on a 1/12 octave grid. This is what the EQ suggestion is
//     fitted against the target profile with.
//  4. Narrow resonances: peaks of a 1/24 octave LTAS above a 1/2 octave
//     smoothed baseline.
//  5. S-sounds: the S frequency is the peak of the sibilant frames' spectrum;
//     a simulation of the plugin's own de-esser detector gives the levels the
//     threshold is chosen from.
#include "Analyzer.h"
#include "FFT.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace va
{
namespace
{
// ---------------------------------------------------------------- helpers
double percentile (std::vector<double> v, double p)
{
    if (v.empty())
        return 0.0;
    std::sort (v.begin(), v.end());
    const double pos = std::clamp (p, 0.0, 1.0) * (double) (v.size() - 1);
    const size_t i = (size_t) pos;
    const double frac = pos - (double) i;
    if (i + 1 < v.size())
        return v[i] * (1.0 - frac) + v[i + 1] * frac;
    return v[i];
}

double powToDb (double p) { return p > 1.0e-30 ? 10.0 * std::log10 (p) : -300.0; }

// Integrates a power spectrum over arbitrary frequency ranges. Each FFT bin is
// treated as a constant power density over its own bandwidth, so bands that
// are narrower than one bin (low frequencies) still work.
class BandIntegrator
{
public:
    void set (const std::vector<double>& power, double binHz_)
    {
        p = &power;
        binHz = binHz_;
        cum.resize (power.size() + 1);
        cum[0] = 0.0;
        for (size_t k = 0; k < power.size(); ++k)
            cum[k + 1] = cum[k] + power[k];
    }

    double cumulative (double f) const
    {
        const double pos = std::clamp (f / binHz + 0.5, 0.0, (double) p->size());
        const size_t k = std::min ((size_t) pos, p->size() - 1);
        const double frac = pos - (double) k;
        return cum[k] + frac * (*p)[k];
    }

    double bandPower (double lo, double hi) const { return std::max (0.0, cumulative (hi) - cumulative (lo)); }
    double density (double lo, double hi) const { return bandPower (lo, hi) / std::max (1.0e-9, hi - lo); }

private:
    const std::vector<double>* p = nullptr;
    std::vector<double> cum;
    double binHz = 1.0;
};

// YIN pitch estimator (de Cheveigne & Kawahara 2002) on a decimated signal.
struct PitchResult
{
    double f0 = 0.0;
    double aperiodicity = 1.0;
};

class Yin
{
public:
    Yin (double fs, double fMin, double fMax, double windowSeconds)
        : sr (fs),
          tauMin (std::max (2, (int) std::floor (fs / fMax))),
          tauMax ((int) std::ceil (fs / fMin)),
          w ((int) std::round (windowSeconds * fs)),
          d ((size_t) tauMax + 2),
          dn ((size_t) tauMax + 2)
    {}

    int samplesNeeded() const { return w + tauMax + 2; }

    PitchResult estimate (const double* x)
    {
        PitchResult r;
        double energy = 0.0;
        for (int j = 0; j < w; ++j)
            energy += x[j] * x[j];
        if (energy < 1.0e-10 * w)
            return r;

        for (int tau = 1; tau <= tauMax + 1; ++tau)
        {
            double s = 0.0;
            for (int j = 0; j < w; ++j)
            {
                const double diff = x[j] - x[j + tau];
                s += diff * diff;
            }
            d[(size_t) tau] = s;
        }

        dn[0] = 1.0;
        double running = 0.0;
        for (int tau = 1; tau <= tauMax + 1; ++tau)
        {
            running += d[(size_t) tau];
            dn[(size_t) tau] = running > 0.0 ? d[(size_t) tau] * tau / running : 1.0;
        }

        constexpr double threshold = 0.15;
        int best = -1;
        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            if (dn[(size_t) tau] < threshold)
            {
                while (tau + 1 <= tauMax && dn[(size_t) tau + 1] < dn[(size_t) tau])
                    ++tau;
                best = tau;
                break;
            }
        }
        if (best < 0)
        {
            best = tauMin;
            for (int tau = tauMin; tau <= tauMax; ++tau)
                if (dn[(size_t) tau] < dn[(size_t) best])
                    best = tau;
        }

        // Parabolic interpolation around the minimum.
        double tauF = best;
        if (best > 1 && best < tauMax + 1)
        {
            const double a = dn[(size_t) best - 1], b = dn[(size_t) best], c = dn[(size_t) best + 1];
            const double den = a - 2.0 * b + c;
            if (std::abs (den) > 1.0e-12)
                tauF = best + std::clamp (0.5 * (a - c) / den, -1.0, 1.0);
        }
        r.f0 = sr / tauF;
        r.aperiodicity = dn[(size_t) best];
        return r;
    }

private:
    double sr;
    int tauMin, tauMax, w;
    std::vector<double> d, dn;
};

std::vector<double> makeGrid()
{
    std::vector<double> g;
    for (int i = 0; i <= 120; ++i)   // 20 Hz .. 20.48 kHz in 1/12 octaves
        g.push_back (20.0 * std::pow (2.0, i / 12.0));
    return g;
}

// Detector envelope simulation, identical ballistics to DeEsser.
std::vector<float> detectorEnvelopeDb (const float* x, size_t n, double fs, double freq, bool bandPass)
{
    Svf bp;
    DeEsser::configureDetector (bp, freq, fs);
    const double att = std::exp (-1.0 / (0.0002 * fs));
    const double rel = std::exp (-1.0 / (0.030 * fs));
    double env = 0.0;
    std::vector<float> out (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double v = std::abs (bandPass ? bp.process (x[i]) : (double) x[i]);
        env = v + (v > env ? att : rel) * (env - v);
        out[i] = (float) gainToDb (env, -150.0);
    }
    return out;
}
} // namespace

// ------------------------------------------------------------ target curves
// Relative 1/3 octave band levels of a well balanced lead vocal (tonal parts).
// Shape: a body region peaking 300-600 Hz, then a gradual slope with a slight
// presence plateau around 2-4 kHz and gently falling air. The values are based
// on long-term speech spectra (Byrne et al. 1994, ANSI S3.5) made a little
// brighter and more forward in 2-5 kHz, as is usual for produced vocals. The
// low end is anchored to the singer's own pitch so a deep voice is not told to
// remove its fundamental.
double targetCurveDb (VoiceType v, double f0, double f)
{
    struct Pt { double f, db; };
    std::vector<Pt> pts;
    if (v == VoiceType::Male)
    {
        if (f0 <= 0.0) f0 = 130.0;
        pts = { { f0 * 0.7, -14.0 }, { f0, -5.0 }, { std::max (f0 * 1.6, 200.0), -1.5 },
                { 300, 0.0 }, { 500, 0.0 }, { 800, -2.5 }, { 1000, -4.0 }, { 1500, -6.5 },
                { 2000, -8.0 }, { 3000, -9.0 }, { 4000, -10.0 }, { 5000, -11.5 }, { 6300, -13.5 },
                { 8000, -16.0 }, { 10000, -18.5 }, { 12500, -21.5 }, { 16000, -26.0 }, { 20000, -34.0 } };
    }
    else
    {
        if (f0 <= 0.0) f0 = 230.0;
        pts = { { f0 * 0.7, -14.0 }, { f0, -4.0 }, { std::max (f0 * 1.5, 350.0), 0.0 },
                { 600, 0.0 }, { 800, -1.5 }, { 1000, -3.0 }, { 1500, -5.5 },
                { 2000, -7.0 }, { 3000, -8.0 }, { 4000, -9.0 }, { 5000, -10.5 }, { 6300, -12.5 },
                { 8000, -15.0 }, { 10000, -17.5 }, { 12500, -20.5 }, { 16000, -25.0 }, { 20000, -33.0 } };
    }
    // Keep the f0-anchored points from crossing the fixed ones.
    std::stable_sort (pts.begin(), pts.end(), [] (const Pt& a, const Pt& b) { return a.f < b.f; });

    if (f <= pts.front().f)
        return pts.front().db - 12.0 * std::log2 (pts.front().f / std::max (1.0, f));  // steep roll-off below the voice
    if (f >= pts.back().f)
        return pts.back().db;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
    {
        if (f <= pts[i + 1].f)
        {
            const double t = std::log (f / pts[i].f) / std::log (pts[i + 1].f / pts[i].f);
            return pts[i].db + t * (pts[i + 1].db - pts[i].db);
        }
    }
    return pts.back().db;
}

// ------------------------------------------------------------------ analyse
AnalysisData analyze (const float* x, size_t n, double fs)
{
    AnalysisData a;
    a.sampleRate = fs;
    a.seconds = (double) n / fs;
    a.gridHz = makeGrid();

    const int N = fs > 50000.0 ? 8192 : 4096;
    const int hop = N / 4;
    if (fs < 16000.0)
    {
        a.error = "Sample rate too low.";
        return a;
    }
    if (n < (size_t) N * 4)
    {
        a.error = "Too little audio was recorded. Play at least 10 seconds of the vocal while analysing.";
        return a;
    }

    double peak = 0.0;
    for (size_t i = 0; i < n; ++i)
        peak = std::max (peak, (double) std::abs (x[i]));
    a.peakDb = gainToDb (peak, -150.0);

    // ---------------------------------------------- decimated copy for pitch
    const int D = std::max (1, (int) std::round (fs / 12000.0));
    const double fsd = fs / D;
    std::vector<double> xd (n / (size_t) D + 1);
    {
        Svf lp1, lp2;
        lp1.setLowPass (0.4 * fsd, 0.54119610, fs);
        lp2.setLowPass (0.4 * fsd, 1.30656296, fs);
        Svf lp3, lp4;
        lp3.setLowPass (0.4 * fsd, 0.54119610, fs);
        lp4.setLowPass (0.4 * fsd, 1.30656296, fs);
        size_t j = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double y = lp4.process (lp3.process (lp2.process (lp1.process (x[i]))));
            if (i % (size_t) D == 0 && j < xd.size())
                xd[j++] = y;
        }
        xd.resize (j);
    }
    Yin yin (fsd, 60.0, 1100.0, 0.035);

    // ------------------------------------------------------ pass 1: frames
    const size_t numFrames = (n - (size_t) N) / (size_t) hop + 1;
    std::vector<double> window ((size_t) N);
    for (int i = 0; i < N; ++i)
        window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * kPi * i / N);
    double winPow = 0.0;
    for (double w : window) winPow += w * w;

    const double binHz = fs / N;
    const double topHz = std::min (0.45 * fs, 20000.0);
    FFT fft (N);

    struct Frame
    {
        double rmsDb = -300.0;
        double f0 = 0.0, aper = 1.0;
        double hfRatio = 0.0, centroid = 0.0;
        bool active = false, tonal = false, sibilant = false;
    };
    std::vector<Frame> frames (numFrames);
    std::vector<double> frameBuf ((size_t) N), power;
    BandIntegrator integ;

    for (size_t fi = 0; fi < numFrames; ++fi)
    {
        const size_t start = fi * (size_t) hop;
        double sumSq = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const double v = x[start + (size_t) i];
            sumSq += v * v;
            frameBuf[(size_t) i] = v * window[(size_t) i];
        }
        auto& fr = frames[fi];
        fr.rmsDb = powToDb (sumSq / N);

        fft.powerSpectrum (frameBuf.data(), power);
        integ.set (power, binHz);
        const double total = integ.bandPower (80.0, topHz);
        const double hf = integ.bandPower (4000.0, std::min (14000.0, topHz));
        fr.hfRatio = total > 0.0 ? hf / total : 0.0;
        double num = 0.0, den = 0.0;
        for (size_t k = (size_t) (80.0 / binHz); k < power.size() && k * binHz < topHz; ++k)
        {
            num += power[k] * k * binHz;
            den += power[k];
        }
        fr.centroid = den > 0.0 ? num / den : 0.0;

        const long centre = (long) ((start + (size_t) N / 2) / (size_t) D);
        const long s0 = centre - yin.samplesNeeded() / 2;
        if (s0 >= 0 && (size_t) (s0 + yin.samplesNeeded()) < xd.size())
        {
            const auto pr = yin.estimate (xd.data() + s0);
            fr.f0 = pr.f0;
            fr.aper = pr.aperiodicity;
        }
    }

    // ------------------------------------------------- classification
    std::vector<double> levels;
    for (const auto& f : frames)
        if (f.rmsDb > -100.0) levels.push_back (f.rmsDb);
    if (levels.empty())
    {
        a.error = "The recording was silent. Press Analyze and then play the vocal track.";
        return a;
    }
    const double loud = percentile (levels, 0.95);
    const double activeGate = std::max (loud - 30.0, -70.0);
    const double hopSec = (double) hop / fs;

    std::vector<double> quiet, activeLevels, f0s;
    for (auto& f : frames)
    {
        f.active = f.rmsDb > activeGate;
        f.sibilant = f.active && f.hfRatio > 0.35 && f.centroid > 3000.0;
        const bool pitched = f.f0 > 0.0 && f.aper < 0.2;
        f.tonal = f.active && pitched && ! f.sibilant && f.hfRatio < 0.25;
        if (f.active) { a.activeSeconds += hopSec; activeLevels.push_back (f.rmsDb); }
        if (f.tonal) { a.voicedSeconds += hopSec; f0s.push_back (std::log (f.f0)); }
        if (f.sibilant) { a.sibilantSeconds += hopSec; ++a.numSibilantFrames; }
        if (f.rmsDb < loud - 40.0) quiet.push_back (f.rmsDb);
    }

    if (a.activeSeconds < 3.0)
    {
        a.error = "Less than 3 seconds of voice was found. Play more of the vocal while analysing.";
        return a;
    }

    // Average level while singing (power mean).
    {
        double s = 0.0;
        for (double l : activeLevels) s += std::pow (10.0, l / 10.0);
        a.activeRmsDb = powToDb (s / (double) activeLevels.size());
    }
    if (quiet.size() > 20)
    {
        a.hasPauses = true;
        a.noiseFloorDb = percentile (quiet, 0.5);
    }

    // ------------------------------------------------------------- pitch
    bool usedFallbackFrames = false;
    if (f0s.size() >= 20)
    {
        a.f0Median = std::exp (percentile (f0s, 0.5));
        a.f0Low = std::exp (percentile (f0s, 0.05));
        a.f0High = std::exp (percentile (f0s, 0.95));
        // Male and female singing ranges overlap. 200 Hz splits typical male
        // (G2-G4) and female (G3-C6) lead vocal medians; the confidence falls
        // off when the median is close to that line.
        const double octavesFromSplit = std::log2 (a.f0Median / 200.0);
        a.detectedVoice = octavesFromSplit < 0.0 ? VoiceType::Male : VoiceType::Female;
        a.voiceConfidence = std::clamp (std::abs (octavesFromSplit) / 0.5, 0.0, 1.0);
    }
    else
    {
        usedFallbackFrames = true;
        for (auto& f : frames)
            f.tonal = f.active && ! f.sibilant;
    }
    (void) usedFallbackFrames;

    // ------------------------------------------- pass 2: long-term spectra
    std::vector<double> tonalSum (power.size(), 0.0), sibSum (power.size(), 0.0), activeSum (power.size(), 0.0);
    int nTonal = 0, nSib = 0, nActive = 0;

    // Harmonic-sampled envelope for resonance detection: every harmonic of every
    // tonal frame is compared with its own frame's smooth spectrum. A room or mic
    // resonance lifts whatever harmonic passes through it, at a fixed frequency,
    // while the harmonics themselves move with the melody.
    const double resLo = 150.0, resHi = std::min (5000.0, 0.4 * fs);
    const int resPerOct = 24;
    const int resBins = (int) std::ceil (std::log2 (resHi / resLo) * resPerOct) + 1;
    std::vector<double> resSum ((size_t) resBins, 0.0), resW ((size_t) resBins, 0.0);
    std::vector<double> hDev, hPos;
    std::vector<double> hAmp;
    for (size_t fi = 0; fi < numFrames; ++fi)
    {
        const auto& fr = frames[fi];
        if (! fr.active)
            continue;
        const size_t start = fi * (size_t) hop;
        for (int i = 0; i < N; ++i)
            frameBuf[(size_t) i] = x[start + (size_t) i] * window[(size_t) i];
        fft.powerSpectrum (frameBuf.data(), power);
        for (size_t k = 0; k < power.size(); ++k)
        {
            const double p = power[k] / winPow;
            activeSum[k] += p;
            if (fr.tonal) tonalSum[k] += p;
            if (fr.sibilant) sibSum[k] += p;
        }
        if (fr.tonal && fr.f0 > 0.0)
        {
            // Level of each harmonic (dB), then compare it with the median of
            // its neighbours h-2, h-1, h+1, h+2 (symmetric, so the spectral
            // slope cancels and a lifted harmonic does not raise its own reference).
            hAmp.clear();
            const int maxH = (int) std::floor (std::min (resHi * 1.3, 0.45 * fs) / fr.f0);
            for (int h = 1; h <= maxH; ++h)
            {
                const size_t k0 = (size_t) std::round (h * fr.f0 / binHz);
                double pk = 0.0;
                for (size_t k = k0 > 0 ? k0 - 1 : 0; k <= k0 + 1 && k < power.size(); ++k) pk = std::max (pk, power[k]);
                hAmp.push_back (powToDb (pk));
            }
            hDev.clear();
            hPos.clear();
            for (int h = 3; h <= maxH; ++h)
            {
                const double fh = h * fr.f0;
                if (fh < resLo * 0.95 || fh > resHi * 1.05 || fh / binHz < 3.0) continue;
                // Neighbours at least ~1/6 of the frequency away, so a resonance
                // wider than the harmonic spacing does not lift its own reference.
                const int k = std::max (1, (int) std::ceil (h / 6.0));
                if (h - 2 * k < 1 || h + 2 * k > maxH) continue;
                double nb[4] = { hAmp[(size_t) (h - 2 * k - 1)], hAmp[(size_t) (h - k - 1)], hAmp[(size_t) (h + k - 1)], hAmp[(size_t) (h + 2 * k - 1)] };
                std::sort (nb, nb + 4);
                const double ref = 0.5 * (nb[1] + nb[2]);
                if (ref < -250.0) continue;
                hDev.push_back (hAmp[(size_t) h - 1] - ref);
                hPos.push_back (std::log2 (fh / resLo) * resPerOct);
            }
            if (hDev.size() >= 4)
            {
                const double mean = std::accumulate (hDev.begin(), hDev.end(), 0.0) / (double) hDev.size();
                for (size_t j = 0; j < hDev.size(); ++j)
                {
                    const double pos = hPos[j];
                    if (pos < 0.0 || pos > resBins - 1) continue;
                    const size_t i0 = (size_t) pos;
                    const double t = pos - (double) i0;
                    resSum[i0] += (1.0 - t) * (hDev[j] - mean); resW[i0] += 1.0 - t;
                    if (i0 + 1 < (size_t) resBins) { resSum[i0 + 1] += t * (hDev[j] - mean); resW[i0 + 1] += t; }
                }
            }
        }
        ++nActive;
        if (fr.tonal) ++nTonal;
        if (fr.sibilant) ++nSib;
    }
    if (nTonal == 0)
    {
        a.error = "No sung or spoken vowels were found in the recording.";
        return a;
    }
    for (auto& v : tonalSum) v /= nTonal;
    for (auto& v : activeSum) v /= std::max (1, nActive);
    if (nSib > 0)
        for (auto& v : sibSum) v /= nSib;

    // 1/3 octave band power on the grid.
    integ.set (tonalSum, binHz);
    const double third = std::pow (2.0, 1.0 / 6.0);
    a.tonalDb.resize (a.gridHz.size());
    for (size_t i = 0; i < a.gridHz.size(); ++i)
    {
        // 1/3 octave equivalent band power. Where harmonics are sparse (low
        // frequencies, high voices) the average is taken over a wider band
        // (at least 1.5 x f0, at most one octave) so the curve shows the
        // voice's envelope rather than single harmonics.
        const double f = a.gridHz[i];
        if (f >= topHz) { a.tonalDb[i] = -300.0; continue; }
        const double thirdWidth = f * (third - 1.0 / third);
        double lo = f / third, hi = f * third;
        const double want = a.f0Median > 0.0 ? 1.5 * a.f0Median : 0.0;
        if (want > thirdWidth)
        {
            const double w = std::min (want, f * (std::sqrt (2.0) - 1.0 / std::sqrt (2.0)));
            const double r = (w / f + std::sqrt (w * w / (f * f) + 4.0)) / 2.0;   // geometric band with width w
            lo = f / r; hi = f * r;
        }
        a.tonalDb[i] = powToDb (integ.density (lo, hi) * thirdWidth);
    }

    // Rumble: energy well below the voice's lowest notes.
    {
        BandIntegrator ai;
        ai.set (activeSum, binHz);
        const double cutoff = a.f0Low > 0.0 ? std::min (0.7 * a.f0Low, 90.0) : 60.0;
        const double total = ai.bandPower (20.0, topHz);
        a.rumbleDb = total > 0.0 ? powToDb (ai.bandPower (15.0, cutoff) / total) : -150.0;
    }

    // ------------------------------------------------------- resonances
    {
        std::vector<double> e ((size_t) resBins, 0.0);
        std::vector<bool> ok ((size_t) resBins, false);
        const double minW = std::max (12.0, 0.02 * nTonal);
        for (int i = 0; i < resBins; ++i)
            if (resW[(size_t) i] >= minW) { ok[(size_t) i] = true; e[(size_t) i] = resSum[(size_t) i] / resW[(size_t) i]; }

        auto freqOf = [&] (int i) { return resLo * std::pow (2.0, (double) i / resPerOct); };
        std::vector<double> excess ((size_t) resBins, 0.0);
        for (int i = 0; i < resBins; ++i)
        {
            if (! ok[(size_t) i]) continue;
            double sum = 0.0, cnt = 0.0;
            for (int j = i - resPerOct / 3; j <= i + resPerOct / 3; ++j)   // +-1/3 octave baseline
            {
                if (j < 0 || j >= resBins || std::abs (j - i) <= 1 || ! ok[(size_t) j]) continue;
                sum += e[(size_t) j]; cnt += 1.0;
            }
            excess[(size_t) i] = cnt >= 6.0 ? e[(size_t) i] - sum / cnt : 0.0;
        }

        std::vector<Resonance> found;
        for (int i = 1; i + 1 < resBins; ++i)
        {
            const double x = excess[(size_t) i];
            if (x < 3.0 || x < excess[(size_t) i - 1] || x < excess[(size_t) i + 1]) continue;
            int lo = i, hi = i;
            while (lo > 0 && excess[(size_t) lo - 1] > x * 0.5) --lo;
            while (hi + 1 < resBins && excess[(size_t) hi + 1] > x * 0.5) ++hi;
            const double bwOct = std::max (1.0 / 24.0, (double) (hi - lo + 1) / resPerOct);
            const double r = std::pow (2.0, bwOct);
            Resonance res;
            res.freq = freqOf (i);
            res.excessDb = x;
            res.q = std::clamp (std::sqrt (r) / (r - 1.0), 2.0, 10.0);
            // Little pitch variety means few samples per frequency: less certain.
            res.uncertain = resW[(size_t) i] < 3.0 * minW || (a.f0Low > 0.0 && a.f0High / a.f0Low < 1.3);
            found.push_back (res);
        }
        std::sort (found.begin(), found.end(), [] (const Resonance& p, const Resonance& q) { return p.excessDb > q.excessDb; });
        if (found.size() > 3) found.resize (3);
        a.resonances = found;
    }

    // ------------------------------------------------------ S-sounds
    a.sibilantDb.assign (a.gridHz.size(), -300.0);
    if (nSib > 0)
    {
        BandIntegrator si;
        si.set (sibSum, binHz);
        for (size_t i = 0; i < a.gridHz.size(); ++i)
        {
            const double f = a.gridHz[i];
            if (f < topHz) a.sibilantDb[i] = powToDb (si.density (f / third, f * third));
        }
        // Peak of the S spectrum (smoothed 1/3 octave), 3-12 kHz.
        double best = -1e9;
        for (size_t i = 0; i < a.gridHz.size(); ++i)
        {
            const double f = a.gridHz[i];
            if (f < 3000.0 || f > std::min (12000.0, 0.42 * fs)) continue;
            if (a.sibilantDb[i] > best) { best = a.sibilantDb[i]; a.sibFreq = f; }
        }
    }

    // Detector simulation at the S frequency, plus a wide-band peak envelope.
    {
        const auto det = detectorEnvelopeDb (x, n, fs, a.sibFreq, true);
        const auto full = detectorEnvelopeDb (x, n, fs, a.sibFreq, false);
        std::vector<double> sibDet, vowDet, vowPeak;
        for (size_t fi = 0; fi < numFrames; ++fi)
        {
            const auto& fr = frames[fi];
            if (! fr.active) continue;
            const size_t c = fi * (size_t) hop + (size_t) N / 2;
            const size_t s0 = c - (size_t) hop / 2, s1 = std::min (n, c + (size_t) hop / 2);
            float m = -150.0f, mf = -150.0f;
            for (size_t i = s0; i < s1; ++i) { m = std::max (m, det[i]); mf = std::max (mf, full[i]); }
            if (fr.sibilant) sibDet.push_back (m);
            // Vowels right next to an S share frames with it; leave them out.
            bool nearS = false;
            for (size_t k = fi >= 2 ? fi - 2 : 0; k <= std::min (numFrames - 1, fi + 2); ++k)
                nearS = nearS || frames[k].sibilant;
            if (fr.tonal && ! nearS) { vowDet.push_back (m); vowPeak.push_back (mf); }
        }
        if (! sibDet.empty())
        {
            a.sibDetP50 = percentile (sibDet, 0.5);
            a.sibDetP90 = percentile (sibDet, 0.9);
        }
        if (! vowDet.empty())
        {
            a.vowelDetP99 = percentile (vowDet, 0.98);
            a.vowelPeakP90 = percentile (vowPeak, 0.9);
        }
    }

    a.valid = true;
    return a;
}

// ------------------------------------------------------------------ suggest
namespace
{
double fitWeight (double f)
{
    if (f < 150.0) return 0.6;
    if (f > 10000.0) return 0.5;
    if (f > 6000.0) return 0.8;
    return 1.0;
}

std::string verdictFor (double dev)
{
    const double a = std::abs (dev);
    if (a < 1.5) return "balanced";
    const char* dir = dev > 0 ? "too much" : "too little";
    if (a < 3.0) return std::string ("slightly ") + dir;
    if (a < 6.0) return dir;
    return std::string ("clearly ") + dir;
}

std::string fmt (double v, int decimals = 1)
{
    std::ostringstream o;
    o << std::fixed << std::setprecision (decimals) << v;
    return o.str();
}

std::string fmtHz (double f)
{
    if (f >= 1000.0) return fmt (f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz";
    return fmt (f, 0) + " Hz";
}
} // namespace

Suggestion suggest (const AnalysisData& a, ProfileChoice choice, double amount, const AnalysisData* reference)
{
    Suggestion s;
    if (! a.valid)
        return s;
    s.valid = true;
    s.amount = amount;
    s.profile = choice == ProfileChoice::Auto ? a.detectedVoice
              : choice == ProfileChoice::Male ? VoiceType::Male : VoiceType::Female;
    const bool male = s.profile == VoiceType::Male;
    const double fs = a.sampleRate;
    const auto& grid = a.gridHz;
    const size_t G = grid.size();

    // f0 to anchor the target with: the measured one if it fits the profile,
    // otherwise a typical value for the chosen profile.
    double f0 = a.f0Median;
    if (f0 <= 0.0 || (male && f0 > 300.0) || (! male && f0 < 140.0))
        f0 = male ? 130.0 : 230.0;
    const double f0Low = a.f0Low > 0.0 ? std::min (a.f0Low, f0) : f0 * 0.75;

    // ---- high pass
    auto& eq = s.eq;
    eq = EqSettings::defaults();
    const double hpf = male ? std::clamp (0.75 * f0Low, 50.0, 120.0) : std::clamp (0.75 * f0Low, 70.0, 180.0);
    eq.bands[kHpf] = { true, hpf, 0.0, 0.707 };
    eq.hpfSteep = a.rumbleDb > -30.0;

    // ---- target and deviation
    const double fitLo = std::max (hpf * 1.4, f0 * 0.9);
    const double fitHi = std::min (16000.0, 0.45 * fs);
    std::vector<double> target (G), measured (G);
    for (size_t i = 0; i < G; ++i)
    {
        // A reference vocal's own curve can replace the built-in profile.
        const bool useRef = reference != nullptr && reference->valid && reference->tonalDb.size() == G && reference->tonalDb[i] > -250.0;
        target[i] = useRef ? reference->tonalDb[i] : targetCurveDb (s.profile, f0, grid[i]);
        measured[i] = a.tonalDb[i];
    }
    // Align levels (median difference over 250 Hz - 4 kHz).
    std::vector<double> diffs;
    for (size_t i = 0; i < G; ++i)
        if (grid[i] >= 200.0 && grid[i] <= std::min (8000.0, 0.4 * fs))
            diffs.push_back (measured[i] - target[i]);
    const double offset = percentile (diffs, 0.5);
    for (size_t i = 0; i < G; ++i)
        target[i] += offset;
    s.targetDb = target;
    s.measuredDb = measured;

    std::vector<double> want (G, 0.0), w (G, 0.0);   // desired correction and fit weight
    for (size_t i = 0; i < G; ++i)
    {
        if (grid[i] < fitLo || grid[i] > fitHi || measured[i] < -250.0)
            continue;
        // Dead band: deviations within +-1 dB are left alone. Around the
        // fundamental, voices differ a lot (a strong fundamental is normal,
        // especially for high voices), so up to +6 dB is accepted there.
        double d = target[i] - measured[i];
        const double tolCut = grid[i] < 1.6 * f0 ? 6.0 : 1.0;
        d = d > 1.0 ? d - 1.0 : (d < -tolCut ? d + tolCut : 0.0);
        want[i] = std::clamp (d, -10.0, 7.0);
        w[i] = fitWeight (grid[i]) * (grid[i] < 1.6 * f0 ? 0.5 : 1.0);
    }

    // ---- resonances first (narrow cuts)
    int bell = kBell1;
    for (const auto& r : a.resonances)
    {
        if (bell > kBell2) break;   // at most two bells for resonances
        if (r.excessDb < 6.0 || r.uncertain) continue;   // only clear, well-supported peaks are cut
        eq.bands[(size_t) bell] = { true, r.freq, -std::min (6.0, 0.6 * r.excessDb), r.q };
        ++bell;
    }
    const int firstTonalBell = bell;

    // ---- shelves
    const double lsFreq = std::clamp (1.6 * f0, male ? 160.0 : 250.0, male ? 260.0 : 450.0);
    const double hsFreq = std::min (10000.0, 0.35 * fs);
    auto meanWant = [&] (double lo, double hi)
    {
        double sw = 0.0, sv = 0.0;
        for (size_t i = 0; i < G; ++i)
            if (grid[i] >= lo && grid[i] <= hi && w[i] > 0.0) { sw += w[i]; sv += w[i] * want[i]; }
        return sw > 0.0 ? sv / sw : 0.0;
    };
    eq.bands[kLowShelf] = { true, lsFreq, std::clamp (meanWant (fitLo, lsFreq * 0.8), -6.0, 4.0), 0.707 };
    // If the top end is missing almost completely (dark mic, low bit-rate or
    // band-limited source) a big boost would mostly lift noise: cap it.
    double airDeficit = 0.0;
    {
        int c = 0;
        for (size_t i = 0; i < G; ++i)
            if (grid[i] >= 10000.0 && grid[i] <= fitHi && measured[i] > -250.0) { airDeficit += target[i] - measured[i]; ++c; }
        airDeficit = c > 0 ? airDeficit / c : 0.0;
    }
    const double hsMax = airDeficit > 10.0 ? 3.0 : 6.0;
    if (airDeficit > 10.0)
        s.notes.push_back ("Very little energy above 10 kHz (the recording may be dark or band-limited). The air boost is capped at +3 dB to avoid lifting noise.");
    eq.bands[kHighShelf] = { true, hsFreq, fitHi > hsFreq * 1.1 ? std::clamp (meanWant (hsFreq * 1.1, fitHi), -6.0, hsMax) : 0.0, 0.707 };

    auto responseExcluding = [&] (int skip, size_t i)
    {
        double sum = 0.0;
        // Resonance bells are narrow and invisible in the 1/3 octave curve the
        // tonal fit works on, so they are left out here.
        for (int b = kLowShelf; b < kNumBands; ++b)
            if (b != skip && (b < kBell1 || b >= firstTonalBell)) sum += bandResponseDb (eq, b, grid[i], fs);
        return sum;
    };

    // ---- greedy placement of the broad bells
    std::vector<bool> blocked (G, false);
    for (int b = firstTonalBell; b <= kBell4; ++b)
    {
        double bestVal = 0.0;
        size_t bestI = G;
        for (size_t i = 0; i < G; ++i)
        {
            if (w[i] <= 0.0 || blocked[i] || grid[i] < 100.0 || grid[i] > 12000.0) continue;
            const double r = want[i] - responseExcluding (-1, i);
            if (std::abs (r) * w[i] > bestVal) { bestVal = std::abs (r) * w[i]; bestI = i; }
        }
        if (bestI == G || bestVal < 1.0)
            break;
        const double peakR = want[bestI] - responseExcluding (-1, bestI);
        size_t lo = bestI, hi = bestI;
        auto resid = [&] (size_t i) { return want[i] - responseExcluding (-1, i); };
        while (lo > 0 && w[lo - 1] > 0.0 && resid (lo - 1) * peakR > 0.0 && std::abs (resid (lo - 1)) > 0.5 * std::abs (peakR)) --lo;
        while (hi + 1 < G && w[hi + 1] > 0.0 && resid (hi + 1) * peakR > 0.0 && std::abs (resid (hi + 1)) > 0.5 * std::abs (peakR)) ++hi;
        const double bwOct = std::max (1.0 / 3.0, (double) (hi - lo + 1) / 12.0);
        const double r = std::pow (2.0, bwOct);
        const double q = std::clamp (std::sqrt (r) / (r - 1.0), 0.5, 2.0);
        eq.bands[(size_t) b] = { true, grid[bestI], std::clamp (peakR, -9.0, 6.0), q };
        // Do not put another bell within 1/3 octave.
        for (size_t i = 0; i < G; ++i)
            if (std::abs (std::log2 (grid[i] / grid[bestI])) < 1.0 / 3.0) blocked[i] = true;
    }

    // ---- refine all gains together (Gauss-Seidel least squares)
    for (int iter = 0; iter < 12; ++iter)
    {
        for (int b = kLowShelf; b < kNumBands; ++b)
        {
            auto& band = eq.bands[(size_t) b];
            if (! band.enabled) continue;
            if (b == kHighShelf && fitHi <= hsFreq * 1.1) continue;
            const bool resonanceBell = b >= kBell1 && b < firstTonalBell;
            if (resonanceBell) continue;   // keep resonance cuts as measured
            const double unit = std::abs (band.gainDb) > 1.0 ? band.gainDb : 3.0;
            EqSettings probe = eq;
            probe.bands[(size_t) b].gainDb = unit;
            double num = 0.0, den = 0.0;
            for (size_t i = 0; i < G; ++i)
            {
                if (w[i] <= 0.0) continue;
                const double shape = bandResponseDb (probe, b, grid[i], fs) / unit;
                const double t = want[i] - responseExcluding (b, i);
                num += w[i] * t * shape;
                den += w[i] * shape * shape;
            }
            if (den > 1.0e-9)
            {
                const double lim = (b == kLowShelf) ? 6.0 : (b == kHighShelf ? 6.0 : 9.0);
                // A little ridge regularisation prefers small moves when
                // several bands could explain the same deviation.
                band.gainDb = std::clamp (num / (den + 0.6), -lim, b == kLowShelf ? 4.0 : (b == kHighShelf ? hsMax : 6.0));
            }
        }
    }

    // ---- joint pattern search over gain, frequency and Q of the tonal bands
    // Cost: weighted squared error to the wanted correction plus a small
    // penalty on gain, so the simplest EQ that does the job wins.
    {
        std::vector<int> tunable;
        for (int b = kLowShelf; b < kNumBands; ++b)
        {
            if (! eq.bands[(size_t) b].enabled) continue;
            if (b >= kBell1 && b < firstTonalBell) continue;
            if (b == kHighShelf && fitHi <= hsFreq * 1.1) continue;
            tunable.push_back (b);
        }
        auto cost = [&] (const EqSettings& e)
        {
            double c = 0.0;
            for (size_t i = 0; i < G; ++i)
            {
                if (w[i] <= 0.0) continue;
                double r = want[i];
                for (int b : tunable) r -= bandResponseDb (e, b, grid[i], fs);
                c += w[i] * r * r;
            }
            for (int b : tunable) c += 0.25 * e.bands[(size_t) b].gainDb * e.bands[(size_t) b].gainDb;
            // Bells closer than half an octave just fight each other.
            for (int b1 : tunable)
                for (int b2 : tunable)
                    if (b1 < b2 && b1 >= kBell1 && b2 <= kBell4
                        && std::abs (std::log2 (e.bands[(size_t) b1].freq / e.bands[(size_t) b2].freq)) < 0.5)
                        c += 40.0;
            return c;
        };
        auto clampBand = [&] (int b, BandSettings& band)
        {
            if (b == kLowShelf) { band.gainDb = std::clamp (band.gainDb, -6.0, 4.0); return; }
            if (b == kHighShelf) { band.gainDb = std::clamp (band.gainDb, -6.0, hsMax); return; }
            band.gainDb = std::clamp (band.gainDb, -9.0, 6.0);
            band.freq = std::clamp (band.freq, 100.0, 12000.0);
            band.q = std::clamp (band.q, 0.5, 2.0);
        };
        double best = cost (eq);
        double gStep = 1.0, fStep = std::pow (2.0, 1.0 / 6.0), qStep = 1.3;
        for (int round = 0; round < 60 && gStep > 0.05; ++round)
        {
            bool improved = false;
            for (int b : tunable)
            {
                const bool bellBand = b >= kBell1 && b <= kBell4;
                for (int param = 0; param < (bellBand ? 3 : 1); ++param)
                    for (int dir : { -1, 1 })
                    {
                        EqSettings trial = eq;
                        auto& band = trial.bands[(size_t) b];
                        if (param == 0) band.gainDb += dir * gStep;
                        else if (param == 1) band.freq *= dir > 0 ? fStep : 1.0 / fStep;
                        else band.q *= dir > 0 ? qStep : 1.0 / qStep;
                        clampBand (b, band);
                        const double c = cost (trial);
                        if (c < best - 1.0e-9) { best = c; eq = trial; improved = true; }
                    }
            }
            if (! improved)
            {
                gStep *= 0.5;
                fStep = std::sqrt (fStep);
                qStep = std::sqrt (qStep);
            }
        }
    }

    // Drop bands that do (almost) nothing.
    for (int b = kLowShelf; b < kNumBands; ++b)
    {
        auto& band = eq.bands[(size_t) b];
        if (band.enabled && std::abs (band.gainDb) < 1.0)
        {
            band.enabled = false;
            band.gainDb = 0.0;
        }
    }

    // Unused bells keep sensible starting positions but stay off.
    const double defaultBells[] = { 300.0, 1000.0, 3000.0, 6000.0 };
    for (int b = kBell1; b <= kBell4; ++b)
        if (! eq.bands[(size_t) b].enabled)
            eq.bands[(size_t) b] = { false, defaultBells[b - kBell1], 0.0, 1.0 };

    // ---- amount
    for (int b = kLowShelf; b < kNumBands; ++b)
        eq.bands[(size_t) b].gainDb *= amount;

    // ---- problem areas (measured relative to target, before EQ)
    auto meanDev = [&] (double lo, double hi)
    {
        double sum = 0.0;
        int c = 0;
        for (size_t i = 0; i < G; ++i)
            if (grid[i] >= lo && grid[i] <= hi && grid[i] >= fitLo * 0.95 && grid[i] <= fitHi && measured[i] > -250.0)
            { sum += measured[i] - target[i]; ++c; }
        return c > 0 ? sum / c : 0.0;
    };
    struct AreaDef { const char* name; double lo, hi; };
    const AreaDef defs[] = {
        { "Low end / proximity", f0 * 0.9, std::max (f0 * 1.6, male ? 200.0 : 300.0) },
        { "Muddiness", std::max (f0 * 1.6, male ? 200.0 : 300.0), 500.0 },
        { "Boxiness", std::max (f0 * 1.6, 500.0), 900.0 },
        { "Nasal", 900.0, 1600.0 },
        { "Presence / harshness", 2000.0, 5000.0 },
        { "Air", 10000.0, 16000.0 },
    };
    for (const auto& d : defs)
    {
        if (d.hi <= d.lo || d.lo > fitHi) continue;
        ProblemArea p;
        p.name = d.name;
        p.range = fmtHz (d.lo) + " - " + fmtHz (d.hi);
        p.deviationDb = meanDev (d.lo, d.hi);
        p.verdict = verdictFor (p.deviationDb);
        s.areas.push_back (p);
    }

    // ---- de-esser
    auto& ds = s.deess;
    ds.enabled = true;
    ds.focused = false;
    ds.freq = std::clamp (a.sibFreq, 3000.0, 12000.0);
    const double eqAtS = eqResponseDb (eq, ds.freq, fs);   // EQ comes before the de-esser
    constexpr double slope = 1.0 - 1.0 / DeEsser::kRatio;
    if (a.numSibilantFrames >= 10 && a.sibDetP90 > -140.0)
    {
        // Wanted: S peaks (90th percentile) about 10 dB under the vowel peaks.
        const double excess = a.sibDetP90 - (a.vowelPeakP90 - 10.0);
        const double reduce = std::clamp (excess, 2.0, 10.0);
        double thr = a.sibDetP90 - reduce / slope;
        const double vowelGuard = a.vowelDetP99 + 1.0;
        if (thr < vowelGuard)
        {
            thr = vowelGuard;
            s.notes.push_back ("The S-sounds are not much louder than the vowels in the S region, so the de-esser "
                               "threshold is kept above the vowels. Listen for dull vowels if you lower it.");
        }
        ds.thresholdDb = thr + eqAtS;
        ds.rangeDb = std::clamp (reduce + 3.0, 4.0, 12.0);
    }
    else
    {
        ds.thresholdDb = std::max (-40.0, a.vowelDetP99 + 3.0) + eqAtS;
        ds.rangeDb = 6.0;
        s.notes.push_back ("Few S-sounds were found, so the de-esser settings are a gentle starting point.");
    }
    ds.thresholdDb = std::clamp (ds.thresholdDb, -60.0, 0.0);

    // ---- notes
    if (a.peakDb > -0.3)
        s.notes.push_back ("The recording reaches 0 dBFS. Check for clipping/distortion; EQ cannot repair it.");
    if (a.hasPauses && a.activeRmsDb - a.noiseFloorDb < 35.0)
        s.notes.push_back ("Background noise in the pauses is only " + fmt (a.activeRmsDb - a.noiseFloorDb, 0)
                           + " dB below the voice. Consider a gate or noise reduction; EQ boosts will raise the noise too.");
    if (eq.hpfSteep)
        s.notes.push_back ("Clear low-frequency rumble or plosives below the voice: the low cut is set to 24 dB/oct.");
    if (choice == ProfileChoice::Auto && a.f0Median > 0.0 && a.voiceConfidence < 0.4)
        s.notes.push_back ("The pitch range is between a typical male and female voice; try both profiles and keep the one that sounds best.");
    if (a.f0Median <= 0.0)
        s.notes.push_back ("No clear pitch was found, so the profile uses typical values. Choose Male or Female manually.");
    for (const auto& r : a.resonances)
        if (r.uncertain)
            s.notes.push_back ("The peak at " + fmtHz (r.freq) + " could not be confirmed (too little pitch variation in the take), so it is not cut automatically.");
    if (a.activeSeconds < 15.0)
        s.notes.push_back ("Only " + fmt (a.activeSeconds, 0) + " s of voice was analysed. 30-60 s gives a more reliable result.");

    return s;
}

// ------------------------------------------------------------------ report
std::string formatReport (const AnalysisData& a, const Suggestion& s)
{
    std::ostringstream o;
    if (! a.valid)
    {
        o << "Analysis failed: " << a.error << "\n";
        return o.str();
    }
    o << "ANALYSIS (" << fmt (a.activeSeconds, 0) << " s of voice out of " << fmt (a.seconds, 0) << " s)\n";
    if (a.f0Median > 0.0)
        o << "Pitch: median " << fmtHz (a.f0Median) << ", range " << fmtHz (a.f0Low) << " - " << fmtHz (a.f0High) << "\n"
          << "Detected voice: " << (a.detectedVoice == VoiceType::Male ? "male" : "female")
          << " (confidence " << fmt (a.voiceConfidence * 100.0, 0) << " %)\n";
    else
        o << "Pitch: not measurable\n";
    o << "Profile used: " << (s.profile == VoiceType::Male ? "Male" : "Female") << "\n";
    o << "Level while singing: " << fmt (a.activeRmsDb) << " dBFS RMS, peak " << fmt (a.peakDb) << " dBFS\n\n";

    o << "TONE COMPARED TO TARGET\n";
    for (const auto& p : s.areas)
        o << "  " << p.name << " (" << p.range << "): " << (p.deviationDb >= 0 ? "+" : "") << fmt (p.deviationDb)
          << " dB, " << p.verdict << "\n";
    o << "  Rumble below the voice: " << (a.rumbleDb > -30.0 ? "noticeable" : "low") << " (" << fmt (a.rumbleDb, 0) << " dB)\n";

    if (! a.resonances.empty())
    {
        o << "\nRESONANCES\n";
        for (const auto& r : a.resonances)
            o << "  " << fmtHz (r.freq) << ": stands out " << fmt (r.excessDb) << " dB" << (r.uncertain ? " (uncertain)" : "") << "\n";
    }

    o << "\nS-SOUNDS\n";
    o << "  " << fmt (a.sibilantSeconds, 1) << " s of S-sounds, centred around " << fmtHz (a.sibFreq) << "\n";
    if (a.numSibilantFrames >= 10)
        o << "  S peaks are " << fmt (a.sibDetP90 - a.vowelPeakP90) << " dB relative to vowel peaks\n";

    o << "\nSUGGESTION (amount " << fmt (s.amount * 100.0, 0) << " %)\n";
    const char* names[] = { "Low cut", "Low shelf", "Bell 1", "Bell 2", "Bell 3", "Bell 4", "High shelf" };
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& band = s.eq.bands[(size_t) b];
        if (! band.enabled) continue;
        o << "  " << names[b] << ": " << fmtHz (band.freq);
        if (b == kHpf)
            o << ", " << (s.eq.hpfSteep ? "24" : "12") << " dB/oct";
        else
            o << ", " << (band.gainDb >= 0 ? "+" : "") << fmt (band.gainDb) << " dB" << (b >= kBell1 && b <= kBell4 ? ", Q " + fmt (band.q, 2) : "");
        o << "\n";
    }
    o << "  De-esser: " << fmtHz (s.deess.freq) << ", threshold " << fmt (s.deess.thresholdDb) << " dB, range "
      << fmt (s.deess.rangeDb) << " dB, " << (s.deess.focused ? "focused" : "wide") << "\n";

    if (! s.notes.empty())
    {
        o << "\nNOTES\n";
        for (const auto& n : s.notes)
            o << "  - " << n << "\n";
    }
    return o.str();
}

// ------------------------------------------------------------ serialisation
std::string serialise (const AnalysisData& d)
{
    std::ostringstream o;
    o << std::setprecision (9);
    o << "v1 " << d.valid << ' ' << d.sampleRate << ' ' << d.seconds << ' ' << d.activeSeconds << ' ' << d.voicedSeconds << ' '
      << d.sibilantSeconds << ' ' << d.peakDb << ' ' << d.activeRmsDb << ' ' << d.noiseFloorDb << ' ' << d.hasPauses << ' '
      << d.f0Median << ' ' << d.f0Low << ' ' << d.f0High << ' ' << (int) d.detectedVoice << ' ' << d.voiceConfidence << ' '
      << d.rumbleDb << ' ' << d.numSibilantFrames << ' ' << d.sibFreq << ' ' << d.sibDetP50 << ' ' << d.sibDetP90 << ' '
      << d.vowelDetP99 << ' ' << d.vowelPeakP90 << ' ';
    o << d.gridHz.size() << ' ';
    for (size_t i = 0; i < d.gridHz.size(); ++i)
        o << d.gridHz[i] << ' ' << d.tonalDb[i] << ' ' << d.sibilantDb[i] << ' ';
    o << d.resonances.size() << ' ';
    for (const auto& r : d.resonances)
        o << r.freq << ' ' << r.excessDb << ' ' << r.q << ' ' << r.uncertain << ' ';
    return o.str();
}

bool deserialise (const std::string& text, AnalysisData& d)
{
    std::istringstream in (text);
    std::string ver;
    in >> ver;
    if (ver != "v1") return false;
    AnalysisData r;
    int voice = 0;
    size_t g = 0, nr = 0;
    in >> r.valid >> r.sampleRate >> r.seconds >> r.activeSeconds >> r.voicedSeconds >> r.sibilantSeconds >> r.peakDb
       >> r.activeRmsDb >> r.noiseFloorDb >> r.hasPauses >> r.f0Median >> r.f0Low >> r.f0High >> voice >> r.voiceConfidence
       >> r.rumbleDb >> r.numSibilantFrames >> r.sibFreq >> r.sibDetP50 >> r.sibDetP90 >> r.vowelDetP99 >> r.vowelPeakP90 >> g;
    if (! in || g > 1000) return false;
    r.detectedVoice = voice == 1 ? VoiceType::Female : VoiceType::Male;
    r.gridHz.resize (g); r.tonalDb.resize (g); r.sibilantDb.resize (g);
    for (size_t i = 0; i < g; ++i)
        in >> r.gridHz[i] >> r.tonalDb[i] >> r.sibilantDb[i];
    in >> nr;
    if (! in || nr > 16) return false;
    r.resonances.resize (nr);
    for (auto& res : r.resonances)
        in >> res.freq >> res.excessDb >> res.q >> res.uncertain;
    if (! in) return false;
    d = r;
    return true;
}
} // namespace va
