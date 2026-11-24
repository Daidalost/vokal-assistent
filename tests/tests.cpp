// Vokal Assistent - unit and closed-loop tests for the DSP and the analyser.
#include "../src/core/Analyzer.h"
#include "../src/core/DeEsser.h"
#include "../src/core/VocalEQ.h"
#include "Synth.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace va;
using namespace vatest;

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++checks;                                                              \
        if (! (cond)) {                                                        \
            ++failures;                                                        \
            std::printf ("  FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond);    \
            std::printf (__VA_ARGS__);                                         \
            std::printf ("\n");                                                \
        }                                                                      \
    } while (0)

static double rmsDb (const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    if (to == 0) to = x.size();
    double s = 0.0;
    for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
    return 10.0 * std::log10 (s / (double) (to - from) + 1e-30);
}

static std::vector<float> sine (double f, double fs, double seconds, double amp = 0.5)
{
    std::vector<float> v ((size_t) (seconds * fs));
    for (size_t i = 0; i < v.size(); ++i) v[i] = (float) (amp * std::sin (2.0 * kPi * f * (double) i / fs));
    return v;
}

static std::vector<float> applyEq (const EqSettings& s, const std::vector<float>& x, double fs)
{
    VocalEqChannel ch;
    ch.prepare (fs);
    ch.setSettings (s);
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = (float) ch.process (x[i]);
    return y;
}

static std::vector<float> applyDeEss (const DeEssSettings& s, std::vector<float> x, double fs)
{
    DeEsser d;
    d.prepare (fs, 1);
    d.setSettings (s);
    float* chans[1] = { x.data() };
    d.process (chans, 1, (int) x.size());
    return x;
}

// ---------------------------------------------------------------------------
static void testFilterResponses()
{
    std::printf ("Filter responses match analytic curves\n");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        struct Case { FilterType t; double fc, q, g; };
        const Case cases[] = {
            { FilterType::Bell, 1000, 1.0, 6.0 }, { FilterType::Bell, 3000, 4.0, -9.0 }, { FilterType::Bell, 12000, 0.7, 5.0 },
            { FilterType::LowShelf, 200, 0.707, -4.0 }, { FilterType::LowShelf, 300, 0.707, 5.0 },
            { FilterType::HighShelf, 10000, 0.707, 4.0 }, { FilterType::HighShelf, 4000, 0.707, -7.0 },
            { FilterType::HighPass12, 100, 0.70710678, 0 }, { FilterType::HighPass24, 100, 0, 0 },
            { FilterType::BandPass, 6500, 1.0, 0 },
        };
        for (const auto& c : cases)
        {
            for (double f : { 50.0, 100.0, 200.0, 700.0, 1000.0, 2500.0, 6000.0, 11000.0, 16000.0 })
            {
                Svf a, b;
                switch (c.t)
                {
                    case FilterType::Bell: a.setBell (c.fc, c.q, c.g, fs); break;
                    case FilterType::LowShelf: a.setLowShelf (c.fc, c.q, c.g, fs); break;
                    case FilterType::HighShelf: a.setHighShelf (c.fc, c.q, c.g, fs); break;
                    case FilterType::HighPass12: a.setHighPass (c.fc, c.q, fs); break;
                    case FilterType::HighPass24: a.setHighPass (c.fc, 0.54119610, fs); b.setHighPass (c.fc, 1.30656296, fs); break;
                    case FilterType::BandPass: a.setBandPass (c.fc, c.q, fs); break;
                    default: break;
                }
                // Measure the steady-state gain with a quadrature (I/Q) correlation.
                const size_t warm = (size_t) (0.5 * fs), len = (size_t) (0.5 * fs);
                double ci = 0, cq = 0;
                for (size_t i = 0; i < warm + len; ++i)
                {
                    const double ph = 2.0 * kPi * f * (double) i / fs;
                    double y = a.process (std::sin (ph));
                    if (c.t == FilterType::HighPass24) y = b.process (y);
                    if (i >= warm) { ci += y * std::sin (ph); cq += y * std::cos (ph); }
                }
                const double measured = 20.0 * std::log10 (2.0 * std::sqrt (ci * ci + cq * cq) / (double) len);
                const double expected = magnitudeDb (c.t, c.fc, c.q, c.g, f, fs);
                if (expected > -60.0)
                    CHECK (std::abs (measured - expected) < 0.05, "type %d fc %.0f f %.0f fs %.0f: measured %.3f expected %.3f",
                           (int) c.t, c.fc, f, fs, measured, expected);
            }
        }
    }
    // Sanity on the analytic curves themselves.
    CHECK (std::abs (magnitudeDb (FilterType::Bell, 1000, 1.0, 6.0, 1000, 48000) - 6.0) < 0.01, "bell centre");
    CHECK (std::abs (magnitudeDb (FilterType::LowShelf, 200, 0.707, -4.0, 30, 48000) + 4.0) < 0.2, "low shelf plateau");
    CHECK (std::abs (magnitudeDb (FilterType::HighShelf, 8000, 0.707, 5.0, 20000, 48000) - 5.0) < 0.3, "high shelf plateau");
    CHECK (std::abs (magnitudeDb (FilterType::HighPass12, 100, 0.70710678, 0, 100, 48000) + 3.01) < 0.05, "hpf -3 dB point");
}

static void testEqIdentity()
{
    std::printf ("EQ with flat settings is transparent\n");
    EqSettings s = EqSettings::defaults();
    for (int b = kLowShelf; b < kNumBands; ++b) { s.bands[(size_t) b].enabled = true; s.bands[(size_t) b].gainDb = 0.0; }
    auto x = sine (440.0, 48000, 0.5);
    auto y = applyEq (s, x, 48000);
    double maxErr = 0;
    for (size_t i = 0; i < x.size(); ++i) maxErr = std::max (maxErr, (double) std::abs (x[i] - y[i]));
    CHECK (maxErr < 1e-6, "max error %g", maxErr);
}

static void testDeEsser()
{
    std::printf ("De-esser behaviour\n");
    // Gain computer.
    CHECK (DeEsser::computeReductionDb (-40, -30, 10) == 0.0, "below threshold");
    CHECK (std::abs (DeEsser::computeReductionDb (-18, -30, 20) - 9.0) < 1e-9, "12 dB over at 4:1 -> 9 dB");
    CHECK (std::abs (DeEsser::computeReductionDb (0, -30, 6) - 6.0) < 1e-9, "range ceiling");
    CHECK (DeEsser::computeReductionDb (-30, -30, 10) > 0.0 && DeEsser::computeReductionDb (-30, -30, 10) < 1.0, "soft knee");

    const double fs = 48000;
    DeEssSettings s;
    s.enabled = true; s.freq = 7000; s.thresholdDb = -30; s.rangeDb = 10;
    for (bool focused : { false, true })
    {
        s.focused = focused;
        // Loud S tone (-6 dBFS at 7 kHz) is reduced by about the range.
        auto hi = sine (7000, fs, 1.0, 0.5);
        auto y = applyDeEss (s, hi, fs);
        const double red = rmsDb (hi, 24000) - rmsDb (y, 24000);
        CHECK (red > 8.0 && red < 10.5, "focused=%d 7 kHz reduction %.2f dB", (int) focused, red);
        // Low vowel tone at the same level is not touched (the band pass ignores it).
        auto lo = sine (300, fs, 1.0, 0.5);
        auto yl = applyDeEss (s, lo, fs);
        double maxErr = 0;
        for (size_t i = 0; i < lo.size(); ++i) maxErr = std::max (maxErr, (double) std::abs (lo[i] - yl[i]));
        CHECK (maxErr < 1e-4, "focused=%d vowel changed by %g", (int) focused, maxErr);
        // Quiet S below threshold is untouched.
        auto q = sine (7000, fs, 1.0, 0.005);
        auto yq = applyDeEss (s, q, fs);
        CHECK (std::abs (rmsDb (q, 24000) - rmsDb (yq, 24000)) < 0.05, "quiet S untouched");
        // Listen output + normal output = input.
        DeEssSettings ls = s; ls.listen = true;
        auto yl2 = applyDeEss (ls, hi, fs);
        double err = 0;
        for (size_t i = 0; i < hi.size(); ++i) err = std::max (err, (double) std::abs (y[i] + yl2[i] - hi[i]));
        CHECK (err < 1e-4, "listen complement error %g", err);
    }
}

// ---------------------------------------------------------------------------
static SynthParams male()
{
    SynthParams p;
    p.noteLowHz = 110; p.noteHighHz = 220; p.sibFreq = 6000;
    return p;
}
static SynthParams female()
{
    SynthParams p;
    p.noteLowHz = 220; p.noteHighHz = 523; p.sibFreq = 8000; p.seed = 7;
    return p;
}

static void testPitchAndVoice()
{
    std::printf ("Pitch tracking and voice type\n");
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        auto p = male(); p.fs = fs; p.seconds = 20;
        auto x = synthVocal (p);
        auto a = analyze (x.data(), x.size(), fs);
        CHECK (a.valid, "valid (%s)", a.error.c_str());
        const double expectMedian = std::sqrt (110.0 * 220.0);
        CHECK (a.f0Median > expectMedian * 0.8 && a.f0Median < expectMedian * 1.25, "fs %.0f male median %.1f", fs, a.f0Median);
        CHECK (a.f0Low > 100 && a.f0Low < 125, "male low %.1f", a.f0Low);
        CHECK (a.detectedVoice == VoiceType::Male, "male detected");
        CHECK (std::abs (a.sibFreq / 6000.0 - 1.0) < 0.2, "male S freq %.0f", a.sibFreq);
    }
    auto p = female(); p.seconds = 20;
    auto x = synthVocal (p);
    auto a = analyze (x.data(), x.size(), p.fs);
    CHECK (a.valid, "valid");
    CHECK (a.detectedVoice == VoiceType::Female, "female detected (median %.1f)", a.f0Median);
    CHECK (std::abs (a.sibFreq / 8000.0 - 1.0) < 0.2, "female S freq %.0f", a.sibFreq);
}

static void testResonance()
{
    std::printf ("Resonance detection\n");
    for (double rf : { 900.0, 2400.0, 4300.0 })
    {
        auto p = male();
        p.colour = { { rf, 9.0, 10.0 } };
        auto x = synthVocal (p);
        auto a = analyze (x.data(), x.size(), p.fs);
        bool found = false;
        for (const auto& r : a.resonances)
            if (std::abs (r.freq / rf - 1.0) < 0.05) found = true;
        CHECK (found, "resonance at %.0f not found (%zu found, first %.0f Hz %.1f dB)", rf, a.resonances.size(),
               a.resonances.empty() ? 0.0 : a.resonances[0].freq, a.resonances.empty() ? 0.0 : a.resonances[0].excessDb);
        for (const auto& r : a.resonances) std::printf ("    found %.0f Hz excess %.1f dB Q %.1f%s\n", r.freq, r.excessDb, r.q, r.uncertain ? " (uncertain)" : "");
        auto s = suggest (a, ProfileChoice::Auto);
        double resp = eqResponseDb (s.eq, rf, p.fs);
        CHECK (resp < -3.0, "EQ cut at resonance %.0f: %.1f dB", rf, resp);
    }
    // No strong false positives on a clean voice.
    auto p = male();
    auto x = synthVocal (p);
    auto a = analyze (x.data(), x.size(), p.fs);
    for (const auto& r : a.resonances)
        CHECK (r.excessDb < 6.0 || r.uncertain, "false resonance %.0f Hz %.1f dB", r.freq, r.excessDb);
}

// Difference of suggestions between a coloured and a clean version should undo
// the colouration.
static void testColourCorrection()
{
    std::printf ("Suggestion undoes added colouration\n");
    struct Case { const char* name; SynthParams::Bell b; double probe; };
    const Case cases[] = {
        { "mud +6 dB @ 350 Hz", { 350, 6.0, 1.0 }, 350 },
        { "nasal +6 dB @ 1.2 kHz", { 1200, 6.0, 1.2 }, 1200 },
        { "harsh +5 dB @ 3.5 kHz", { 3500, 5.0, 1.0 }, 3500 },
        { "dull -6 dB @ 8 kHz", { 8000, -6.0, 0.7 }, 8000 },
    };
    for (auto base : { male(), female() })
    {
        auto x0 = synthVocal (base);
        auto a0 = analyze (x0.data(), x0.size(), base.fs);
        auto s0 = suggest (a0, ProfileChoice::Auto);
        for (auto c : cases)
        {
            auto p = base;
            // Around the fundamental the assistant intentionally tolerates extra
            // energy, so for the high voice the low-mid test sits above it.
            if (base.noteLowHz > 160 && c.probe < 400) { c.b.f = 800; c.probe = 800; }
            p.colour = { c.b };
            auto x = synthVocal (p);
            auto a = analyze (x.data(), x.size(), p.fs);
            // The clean take is used as the reference, so the ideal correction is
            // exactly the inverse of the added colouration.
            auto s = suggest (a, ProfileChoice::Auto, 1.0, &a0);
            s0 = suggest (a0, ProfileChoice::Auto, 1.0, &a0);
            // Compare the average correction over the coloured region (half an
            // octave either side) with the average colouration there.
            double diff = 0.0, added = 0.0;
            int cnt = 0;
            for (double f = c.probe / 1.41; f <= c.probe * 1.41; f *= 1.05, ++cnt)
            {
                diff += eqResponseDb (s.eq, f, p.fs) - eqResponseDb (s0.eq, f, p.fs);
                added += magnitudeDb (FilterType::Bell, c.b.f, c.b.q, c.b.gainDb, f, p.fs);
            }
            diff /= cnt; added /= cnt;
            // The assistant is deliberately conservative (1 dB dead band, robust
            // level alignment): it must undo at least half, never overshoot.
            CHECK (diff * added < 0.0 && std::abs (diff) >= 0.5 * std::abs (added) && std::abs (diff) <= std::abs (added) + 1.0, "%s (%s): correction difference %.1f dB (want %.1f)",
                   c.name, base.noteLowHz > 160 ? "female" : "male", diff, -added);
        }
    }
}

// Applying the suggested EQ and analysing again should bring the voice closer
// to the target profile.
static void testClosedLoopEq()
{
    std::printf ("Closed loop: EQ moves the voice towards the target\n");
    auto p = male();
    p.colour = { { 300, 5.0, 1.0 }, { 2500, -4.0, 1.0 }, { 9000, -5.0, 0.7 } };
    auto x = synthVocal (p);
    auto a = analyze (x.data(), x.size(), p.fs);
    auto s = suggest (a, ProfileChoice::Male);
    auto devRms = [] (const Suggestion& sg, const AnalysisData& ad)
    {
        double sum = 0; int c = 0;
        for (size_t i = 0; i < ad.gridHz.size(); ++i)
            if (ad.gridHz[i] > 200 && ad.gridHz[i] < 10000) { const double d = sg.measuredDb[i] - sg.targetDb[i]; sum += d * d; ++c; }
        return std::sqrt (sum / c);
    };
    const double before = devRms (s, a);
    auto y = applyEq (s.eq, x, p.fs);
    auto a2 = analyze (y.data(), y.size(), p.fs);
    auto s2 = suggest (a2, ProfileChoice::Male);
    const double after = devRms (s2, a2);
    CHECK (after < before * 0.6, "deviation before %.2f dB, after %.2f dB", before, after);
    // A second pass should suggest only small changes.
    double maxGain = 0;
    for (int b = kLowShelf; b < kNumBands; ++b)
        if (s2.eq.bands[(size_t) b].enabled) maxGain = std::max (maxGain, std::abs (s2.eq.bands[(size_t) b].gainDb));
    CHECK (maxGain < 3.5, "second pass max gain %.1f dB", maxGain);
}

// The suggested de-esser should bring loud S-sounds down without touching vowels.
static void testClosedLoopDeEss()
{
    std::printf ("Closed loop: de-esser tames loud S-sounds only\n");
    for (auto base : { male(), female() })
    {
        auto p = base;
        p.sibLevelDb = 0.0;   // very sibilant
        auto x = synthVocal (p);
        auto a = analyze (x.data(), x.size(), p.fs);
        auto s = suggest (a, ProfileChoice::Auto);
        CHECK (s.deess.enabled, "de-esser enabled");
        // Process with de-esser only (EQ flat) so the threshold must not include EQ gain.
        EqSettings flat = EqSettings::defaults();
        auto noEq = s; noEq.eq = flat;
        auto aFlat = a;
        auto sFlat = suggest (aFlat, ProfileChoice::Auto, 0.0);   // amount 0 -> EQ gains 0
        auto y = applyDeEss (sFlat.deess, x, p.fs);
        // Measure S frames vs vowel frames using the synth's known S band.
        Svf bpx, bpy;
        bpx.setBandPass (p.sibFreq, 1.0, p.fs);
        bpy.setBandPass (p.sibFreq, 1.0, p.fs);
        double sx = 0, sy = 0, lx = 0, ly = 0;
        Svf lpx, lpy;
        lpx.setLowPass (1500, 0.7, p.fs);
        lpy.setLowPass (1500, 0.7, p.fs);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double hx = bpx.process (x[i]), hy = bpy.process (y[i]);
            sx += hx * hx; sy += hy * hy;
            const double vx = lpx.process (x[i]), vy = lpy.process (y[i]);
            lx += vx * vx; ly += vy * vy;
        }
        const double sRed = 10 * std::log10 (sx / sy), vRed = 10 * std::log10 (lx / ly);
        CHECK (sRed > 3.0, "%s: S band reduced %.1f dB (thr %.1f, range %.1f)", base.noteLowHz > 160 ? "female" : "male", sRed,
               sFlat.deess.thresholdDb, sFlat.deess.rangeDb);
        CHECK (std::abs (vRed) < 0.3, "vowel band changed %.2f dB", vRed);

        // Mild S: little reduction.
        auto pm = base;
        pm.sibLevelDb = -22.0;
        auto xm = synthVocal (pm);
        auto am = analyze (xm.data(), xm.size(), pm.fs);
        auto sm = suggest (am, ProfileChoice::Auto, 0.0);
        auto ym = applyDeEss (sm.deess, xm, pm.fs);
        Svf b1, b2;
        b1.setBandPass (pm.sibFreq, 1.0, pm.fs);
        b2.setBandPass (pm.sibFreq, 1.0, pm.fs);
        double mx = 0, my = 0;
        for (size_t i = 0; i < xm.size(); ++i) { const double u = b1.process (xm[i]), v = b2.process (ym[i]); mx += u * u; my += v * v; }
        const double mRed = 10 * std::log10 (mx / my);
        CHECK (mRed < sRed, "mild S reduced less (%.1f) than strong S (%.1f)", mRed, sRed);
    }
}

static void testRobustness()
{
    std::printf ("Robustness and serialisation\n");
    std::vector<float> silence (48000 * 10, 0.0f);
    auto a = analyze (silence.data(), silence.size(), 48000);
    CHECK (! a.valid && ! a.error.empty(), "silence rejected");
    std::vector<float> shortx (1000, 0.1f);
    a = analyze (shortx.data(), shortx.size(), 48000);
    CHECK (! a.valid, "short rejected");
    // Pure noise: should not crash and give some result or a clear error.
    std::vector<float> noise (48000 * 10);
    std::mt19937 rng (3);
    std::normal_distribution<double> g (0, 0.1);
    for (auto& v : noise) v = (float) g (rng);
    a = analyze (noise.data(), noise.size(), 48000);
    if (a.valid)
    {
        auto s = suggest (a, ProfileChoice::Auto);
        CHECK (s.valid, "noise suggestion valid");
        for (const auto& b : s.eq.bands) CHECK (std::isfinite (b.gainDb) && std::isfinite (b.freq), "finite");
    }

    auto p = male(); p.seconds = 15;
    auto x = synthVocal (p);
    a = analyze (x.data(), x.size(), p.fs);
    AnalysisData b;
    CHECK (deserialise (serialise (a), b), "deserialise");
    auto s1 = suggest (a, ProfileChoice::Auto), s2 = suggest (b, ProfileChoice::Auto);
    double maxDiff = 0;
    for (int i = 0; i < kNumBands; ++i)
        maxDiff = std::max (maxDiff, std::abs (s1.eq.bands[(size_t) i].gainDb - s2.eq.bands[(size_t) i].gainDb));
    CHECK (maxDiff < 1e-6, "round trip suggestion diff %g", maxDiff);
    CHECK (std::abs (s1.deess.thresholdDb - s2.deess.thresholdDb) < 1e-6, "round trip de-ess");
    CHECK (! deserialise ("garbage", b), "garbage rejected");

    // Every suggestion stays within the parameter ranges of the plugin.
    for (const auto& band : s1.eq.bands)
    {
        CHECK (band.freq >= 20 && band.freq <= 20000, "freq range %.0f", band.freq);
        CHECK (band.gainDb >= -15 && band.gainDb <= 15, "gain range %.1f", band.gainDb);
        CHECK (band.q >= 0.3 && band.q <= 10, "q range %.2f", band.q);
    }
    CHECK (s1.deess.thresholdDb >= -60 && s1.deess.thresholdDb <= 0, "thr range");
}

int main (int argc, char** argv)
{
    const std::string only = argc > 1 ? argv[1] : "";
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        { "filters", testFilterResponses }, { "eqidentity", testEqIdentity }, { "deesser", testDeEsser },
        { "pitch", testPitchAndVoice }, { "resonance", testResonance }, { "colour", testColourCorrection },
        { "loopeq", testClosedLoopEq }, { "loopdeess", testClosedLoopDeEss }, { "robust", testRobustness },
    };
    for (const auto& [name, fn] : tests)
        if (only.empty() || only == name) fn();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
