// Vokal Assistent - de-esser (no JUCE dependency)
//
// Detection: a band pass around the S frequency (Q 1.0, about 1.4 octaves wide)
// feeds a peak envelope follower. When the envelope exceeds the threshold, gain
// reduction is computed with a 4:1 ratio, a 6 dB soft knee and a ceiling
// ("range").
//
// Reduction is applied with a dynamically modulated filter, which is an exact
// identity when no reduction is happening (no phase shift on vowels):
//   Wide mode : high shelf starting a little below the S frequency
//               (everything from the S region and up is turned down).
//   Focused   : bell at the S frequency (only the S region is turned down).
// Stereo channels share one detector so the image does not wander.
#pragma once

#include "Filters.h"
#include <vector>
#include <cmath>

namespace va
{
struct DeEssSettings
{
    bool enabled = false;
    double freq = 6500.0;        // centre of the S energy, Hz
    double thresholdDb = -30.0;  // detector level (dBFS) where reduction starts
    double rangeDb = 8.0;        // maximum reduction
    bool focused = false;        // false = Wide (shelf), true = Focused (bell)
    bool listen = false;         // output only what is being removed
};

class DeEsser
{
public:
    static constexpr double kRatio = 4.0;
    static constexpr double kKneeDb = 6.0;
    static constexpr double kDetectorQ = 1.0;
    static constexpr double kBellQ = 1.4;
    static constexpr double kShelfRatio = 1.5;   // shelf corner = freq / 1.5

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        chans.assign ((size_t) std::max (1, numChannels), {});
        envAtt = std::exp (-1.0 / (0.0002 * fs));   // 0.2 ms
        envRel = std::exp (-1.0 / (0.030 * fs));    // 30 ms
        grAtt  = std::exp (-1.0 / (0.0010 * fs));   // 1 ms
        grRel  = std::exp (-1.0 / (0.060 * fs));    // 60 ms
        reset();
        applySettingsToDetectors();
    }

    void reset()
    {
        for (auto& c : chans)
        {
            c.detector.reset();
            c.reducer.reset();
        }
        env = 0.0;
        grDb = 0.0;
        lastAppliedGr = -1.0;
    }

    void setSettings (const DeEssSettings& s)
    {
        const bool retune = s.freq != settings.freq || s.focused != settings.focused;
        settings = s;
        if (retune)
        {
            applySettingsToDetectors();
            lastAppliedGr = -1.0;
        }
    }

    // Static gain computer: detector level (dB) -> reduction (positive dB).
    static double computeReductionDb (double levelDb, double thresholdDb, double rangeDb)
    {
        const double over = levelDb - thresholdDb;
        const double slope = 1.0 - 1.0 / kRatio;
        double r;
        if (over <= -kKneeDb * 0.5)
            r = 0.0;
        else if (over >= kKneeDb * 0.5)
            r = slope * over;
        else
        {
            const double x = over + kKneeDb * 0.5;
            r = slope * x * x / (2.0 * kKneeDb);
        }
        return std::clamp (r, 0.0, std::max (0.0, rangeDb));
    }

    // In-place processing of planar channel data.
    template <typename Sample>
    void process (Sample* const* data, int numChannels, int numSamples)
    {
        numChannels = std::min (numChannels, (int) chans.size());
        if (! settings.enabled)
        {
            currentGr = 0.0;
            grDb = 0.0;
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // --- detection (linked) ---
            double peak = 0.0;
            for (int ch = 0; ch < numChannels; ++ch)
                peak = std::max (peak, std::abs (chans[(size_t) ch].detector.process ((double) data[ch][i])));

            const double coeff = peak > env ? envAtt : envRel;
            env = peak + coeff * (env - peak);
            const double levelDb = gainToDb (env, -150.0);
            const double target = computeReductionDb (levelDb, settings.thresholdDb, settings.rangeDb);
            const double gc = target > grDb ? grAtt : grRel;
            grDb = target + gc * (grDb - target);

            if (std::abs (grDb - lastAppliedGr) > 0.005)
            {
                for (auto& c : chans)
                    configureReducer (c.reducer, grDb);
                lastAppliedGr = grDb;
            }

            // --- reduction ---
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const double x = (double) data[ch][i];
                const double y = chans[(size_t) ch].reducer.process (x);
                data[ch][i] = (Sample) (settings.listen ? (x - y) : y);
            }
        }
        currentGr = grDb;
    }

    double getCurrentReductionDb() const noexcept { return currentGr; }

    // Detector response helpers for the analyser (offline simulation).
    static void configureDetector (Svf& f, double freq, double fs) { f.setBandPass (freq, kDetectorQ, fs); }

private:
    struct Channel
    {
        Svf detector, reducer;
    };

    void applySettingsToDetectors()
    {
        for (auto& c : chans)
            configureDetector (c.detector, settings.freq, fs);
    }

    void configureReducer (Svf& f, double gr) const
    {
        if (settings.focused)
            f.setBell (settings.freq, kBellQ, -gr, fs);
        else
            f.setHighShelf (std::max (1000.0, settings.freq / kShelfRatio), 0.707, -gr, fs);
    }

    double fs = 48000.0;
    DeEssSettings settings;
    std::vector<Channel> chans;
    double envAtt = 0, envRel = 0, grAtt = 0, grRel = 0;
    double env = 0.0, grDb = 0.0, lastAppliedGr = -1.0, currentGr = 0.0;
};
} // namespace va
