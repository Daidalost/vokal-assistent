// Synthetic "vocal" generator for tests: harmonic glottal source with vibrato
// and melody, vowel formants, S-noise bursts, pauses and optional colouration.
#pragma once

#include "../src/core/Filters.h"
#include <random>
#include <vector>
#include <cmath>

namespace vatest
{
struct SynthParams
{
    double fs = 48000.0;
    double seconds = 30.0;
    double noteLowHz = 110.0, noteHighHz = 220.0;   // melody range
    double sibFreq = 6500.0;                        // centre of S noise
    double sibLevelDb = -6.0;                       // S burst level relative to vowel peak level
    double sibEveryNotes = 3;                       // one S after every n notes
    double level = 0.3;
    double sourceSlope = 0.85;                      // harmonic amplitude ~ 1/h^slope                             // vowel peak-ish amplitude
    double noiseFloorDb = -75.0;
    // Colouration applied to the voice (simulating room/mic/proximity):
    struct Bell { double f, gainDb, q; };
    std::vector<Bell> colour;
    unsigned seed = 1;
};

inline std::vector<float> synthVocal (const SynthParams& p)
{
    using namespace va;
    std::mt19937 rng (p.seed);
    std::uniform_real_distribution<double> uni (0.0, 1.0);
    std::normal_distribution<double> gauss (0.0, 1.0);

    const size_t n = (size_t) (p.seconds * p.fs);
    std::vector<float> out (n, 0.0f);

    // Vowel formants (F1..F4, Hz) for a, e, i, o, u (adult averages).
    const double formants[5][4] = { { 730, 1090, 2440, 3400 }, { 530, 1840, 2480, 3500 }, { 300, 2200, 3000, 3700 },
                                    { 570, 840, 2410, 3400 }, { 440, 1020, 2240, 3300 } };
    const double femaleScale = p.noteLowHz > 160.0 ? 1.15 : 1.0;

    Svf micLp; micLp.setLowPass (13000.0, 0.6, p.fs);   // mic/voice roll-off at the very top
    std::vector<Svf> colour (p.colour.size());
    for (size_t i = 0; i < colour.size(); ++i)
        colour[i].setBell (p.colour[i].f, p.colour[i].q, p.colour[i].gainDb, p.fs);

    size_t pos = (size_t) (0.3 * p.fs);
    double phase = 0.0;
    int noteCount = 0;
    while (pos < n)
    {
        // ---- a sung note
        const double dur = 0.35 + 0.5 * uni (rng);
        const size_t len = std::min (n - pos, (size_t) (dur * p.fs));
        const double semis = 12.0 * std::log2 (p.noteHighHz / p.noteLowHz);
        const double f0 = p.noteLowHz * std::pow (2.0, std::floor (uni (rng) * semis) / 12.0);
        const int vowel = (int) (uni (rng) * 5.0) % 5;
        // Formants vary from note to note (different vowels, co-articulation).
        Svf form[4];
        const double fgain[4] = { 10.0, 8.0, 6.0, 5.0 };
        const double fq[4] = { 4.0, 5.0, 6.0, 7.0 };
        for (int k = 0; k < 4; ++k)
            form[k].setBell (formants[vowel][k] * femaleScale * (0.88 + 0.24 * uni (rng)), fq[k], fgain[k], p.fs);
        const int maxH = (int) (0.45 * p.fs / f0);
        const double vibRate = 5.0 + uni (rng), vibDepth = 0.25 * (std::pow (2.0, 1.0 / 12.0) - 1.0);
        for (size_t i = 0; i < len; ++i)
        {
            const double t = (double) i / p.fs;
            const double env = std::min ({ 1.0, t / 0.04, ((double) len / p.fs - t) / 0.06 });
            const double f = f0 * (1.0 + vibDepth * std::sin (2.0 * kPi * vibRate * t) * std::min (1.0, t / 0.2));
            phase += 2.0 * kPi * f / p.fs;
            if (phase > 2.0 * kPi * 1000.0) phase -= 2.0 * kPi * 1000.0;
            double src = 0.0;
            // Source + radiation: about -6 dB/oct per harmonic, the fundamental a
            // little weaker than the 2nd harmonic's neighbourhood (H1-H2 ~ 3 dB).
            for (int h = 1; h <= maxH; ++h)
                src += std::sin (h * phase) * (h == 1 ? 0.7 : 1.0) / std::pow ((double) h, p.sourceSlope);
            src += 0.002 * gauss (rng);   // breath
            double y = src;
            for (auto& fm : form) y = fm.process (y);
            out[pos + i] += (float) (p.level * 0.08 * env * y);
        }
        pos += len;
        ++noteCount;

        // ---- an S between notes
        if (noteCount % (int) p.sibEveryNotes == 0 && pos < n)
        {
            const size_t sl = std::min (n - pos, (size_t) ((0.12 + 0.08 * uni (rng)) * p.fs));
            Svf bp1, bp2;
            bp1.setBandPass (p.sibFreq, 1.2, p.fs);
            bp2.setBandPass (p.sibFreq, 1.2, p.fs);
            const double amp = p.level * std::pow (10.0, p.sibLevelDb / 20.0);
            for (size_t i = 0; i < sl; ++i)
            {
                const double t = (double) i / (double) sl;
                const double env = std::sin (kPi * t);
                out[pos + i] += (float) (amp * env * bp2.process (bp1.process (gauss (rng))) * 1.6);
            }
            pos += sl;
        }
        // ---- occasional pause
        if (uni (rng) < 0.2)
            pos += (size_t) ((0.2 + 0.4 * uni (rng)) * p.fs);
    }

    // Colouration and noise floor.
    const double nf = std::pow (10.0, p.noiseFloorDb / 20.0);
    for (size_t i = 0; i < n; ++i)
    {
        double y = micLp.process (out[i]);
        for (auto& c : colour) y = c.process (y);
        out[i] = (float) (y + nf * gauss (rng));
    }
    return out;
}
} // namespace vatest
