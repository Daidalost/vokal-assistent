// Vokal Assistent - the 7 band vocal EQ (no JUCE dependency)
#pragma once

#include "Filters.h"
#include <array>
#include <vector>

namespace va
{
enum BandIndex
{
    kHpf = 0,
    kLowShelf,
    kBell1,
    kBell2,
    kBell3,
    kBell4,
    kHighShelf,
    kNumBands
};

struct BandSettings
{
    bool enabled = false;
    double freq = 1000.0;
    double gainDb = 0.0;
    double q = 0.707;
};

struct EqSettings
{
    std::array<BandSettings, kNumBands> bands;
    bool hpfSteep = false;   // false = 12 dB/oct, true = 24 dB/oct

    static EqSettings defaults()
    {
        EqSettings s;
        s.bands[kHpf]       = { false, 80.0, 0.0, 0.707 };
        s.bands[kLowShelf]  = { false, 200.0, 0.0, 0.707 };
        s.bands[kBell1]     = { false, 300.0, 0.0, 1.0 };
        s.bands[kBell2]     = { false, 1000.0, 0.0, 1.0 };
        s.bands[kBell3]     = { false, 3000.0, 0.0, 1.0 };
        s.bands[kBell4]     = { false, 6000.0, 0.0, 1.0 };
        s.bands[kHighShelf] = { false, 10000.0, 0.0, 0.707 };
        return s;
    }
};

inline FilterType bandFilterType (int band, bool hpfSteep)
{
    switch (band)
    {
        case kHpf:       return hpfSteep ? FilterType::HighPass24 : FilterType::HighPass12;
        case kLowShelf:  return FilterType::LowShelf;
        case kHighShelf: return FilterType::HighShelf;
        default:         return FilterType::Bell;
    }
}

// Magnitude of one band in dB (0 if disabled).
inline double bandResponseDb (const EqSettings& s, int band, double f, double fs)
{
    const auto& b = s.bands[(size_t) band];
    if (! b.enabled)
        return 0.0;
    if (band != kHpf && std::abs (b.gainDb) < 1.0e-6)
        return 0.0;
    return magnitudeDb (bandFilterType (band, s.hpfSteep), b.freq, b.q, b.gainDb, f, fs);
}

inline double eqResponseDb (const EqSettings& s, double f, double fs)
{
    double sum = 0.0;
    for (int b = 0; b < kNumBands; ++b)
        sum += bandResponseDb (s, b, f, fs);
    return sum;
}

// Per-channel EQ processor. Parameters are smoothed by the caller (the plugin
// updates settings every 32 samples while they move); SVFs are safe to retune.
class VocalEqChannel
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        reset();
    }

    void reset()
    {
        for (auto& f : filters)
            f.reset();
        hpf2.reset();
    }

    void setSettings (const EqSettings& s)
    {
        settings = s;
        for (int b = 0; b < kNumBands; ++b)
        {
            const auto& bs = s.bands[(size_t) b];
            const bool nowActive = bs.enabled;   // a 0 dB bell/shelf is an exact identity
            auto& f = filters[(size_t) b];
            if (nowActive && ! active[(size_t) b])
            {
                f.reset();   // avoid a click from stale state when a band is switched on
                if (b == kHpf)
                    hpf2.reset();
            }
            active[(size_t) b] = nowActive;
            switch (b)
            {
                case kHpf:
                    if (s.hpfSteep)
                    {
                        f.setHighPass (bs.freq, 0.54119610, fs);
                        hpf2.setHighPass (bs.freq, 1.30656296, fs);
                    }
                    else
                    {
                        f.setHighPass (bs.freq, 0.70710678, fs);
                    }
                    break;
                case kLowShelf:  f.setLowShelf (bs.freq, bs.q, bs.gainDb, fs); break;
                case kHighShelf: f.setHighShelf (bs.freq, bs.q, bs.gainDb, fs); break;
                default:         f.setBell (bs.freq, bs.q, bs.gainDb, fs); break;
            }
        }
    }

    inline double process (double x) noexcept
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            if (! active[(size_t) b])
                continue;
            x = filters[(size_t) b].process (x);
            if (b == kHpf && settings.hpfSteep)
                x = hpf2.process (x);
        }
        return x;
    }

private:
    double fs = 48000.0;
    EqSettings settings = EqSettings::defaults();
    std::array<Svf, kNumBands> filters;
    std::array<bool, kNumBands> active {};
    Svf hpf2;
};
} // namespace va
