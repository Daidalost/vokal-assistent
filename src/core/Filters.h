// Vokal Assistent - core filters (no JUCE dependency)
//
// All filters are Andrew Simper's (Cytomic) trapezoidal state-variable filter
// ("SvfLinearTrapOptimised2"). They are numerically robust, safe to modulate per
// sample, and their response is identical to the bilinear-transformed analog
// RBJ prototypes with frequency pre-warping. That lets us compute the exact
// magnitude response analytically (used by the display and by the suggestion
// fitter) with the functions at the bottom of this file.
#pragma once

#include <cmath>
#include <complex>
#include <algorithm>

namespace va
{
constexpr double kPi = 3.14159265358979323846;

inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
inline double gainToDb (double g, double floorDb = -200.0)
{
    return g > 0.0 ? std::max (floorDb, 20.0 * std::log10 (g)) : floorDb;
}

enum class FilterType
{
    HighPass12,   // 2nd order Butterworth high pass
    HighPass24,   // 4th order Butterworth high pass (two cascaded sections)
    LowShelf,
    HighShelf,
    Bell,
    LowPass12,
    BandPass      // constant 0 dB peak gain band pass
};

// One trapezoidal SVF section.
class Svf
{
public:
    void reset() noexcept { ic1 = ic2 = 0.0; }

    // Configure as a plain SVF; outputs mixed as m0*v0 + m1*v1 + m2*v2.
    void setRaw (double g, double k, double m0_, double m1_, double m2_) noexcept
    {
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
        m0 = m0_; m1 = m1_; m2 = m2_;
    }

    void setHighPass (double fc, double q, double fs) noexcept
    {
        const double g = prewarp (fc, fs), k = 1.0 / q;
        setRaw (g, k, 1.0, -k, -1.0);
    }

    void setLowPass (double fc, double q, double fs) noexcept
    {
        const double g = prewarp (fc, fs), k = 1.0 / q;
        setRaw (g, k, 0.0, 0.0, 1.0);
    }

    // Band pass normalised to 0 dB at fc.
    void setBandPass (double fc, double q, double fs) noexcept
    {
        const double g = prewarp (fc, fs), k = 1.0 / q;
        setRaw (g, k, 0.0, k, 0.0);
    }

    void setBell (double fc, double q, double gainDb, double fs) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double g = prewarp (fc, fs), k = 1.0 / (q * A);
        setRaw (g, k, 1.0, k * (A * A - 1.0), 0.0);
    }

    void setLowShelf (double fc, double q, double gainDb, double fs) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double g = prewarp (fc, fs) / std::sqrt (A), k = 1.0 / q;
        setRaw (g, k, 1.0, k * (A - 1.0), A * A - 1.0);
    }

    void setHighShelf (double fc, double q, double gainDb, double fs) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double g = prewarp (fc, fs) * std::sqrt (A), k = 1.0 / q;
        setRaw (g, k, A * A, k * (1.0 - A) * A, 1.0 - A * A);
    }

    inline double process (double v0) noexcept
    {
        const double v3 = v0 - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return m0 * v0 + m1 * v1 + m2 * v2;
    }

    static double prewarp (double fc, double fs) noexcept
    {
        fc = std::clamp (fc, 1.0, 0.4999 * fs);
        return std::tan (kPi * fc / fs);
    }

private:
    double a1 = 1, a2 = 0, a3 = 0, m0 = 1, m1 = 0, m2 = 0;
    double ic1 = 0, ic2 = 0;
};

// ---------------------------------------------------------------------------
// Analytic magnitude response (exactly matches the Svf above).
// The bilinear transform maps digital frequency f to the analog frequency
// tan(pi f / fs); normalising by tan(pi fc / fs) gives the analog prototype
// variable S = j * Omega.
// ---------------------------------------------------------------------------
namespace detail
{
    using cd = std::complex<double>;

    inline cd analogS (double f, double fc, double fs)
    {
        const double nyq = 0.5 * fs;
        f = std::clamp (f, 0.0, nyq * 0.99999);
        fc = std::clamp (fc, 1.0, 0.4999 * fs);
        return { 0.0, std::tan (kPi * f / fs) / std::tan (kPi * fc / fs) };
    }

    inline cd secondOrderHP (cd s, double q) { return (s * s) / (s * s + s / q + 1.0); }
    inline cd secondOrderLP (cd s, double q) { return 1.0 / (s * s + s / q + 1.0); }
}

inline double magnitude (FilterType type, double fc, double q, double gainDb, double f, double fs)
{
    using namespace detail;
    const cd s = analogS (f, fc, fs);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double sqA = std::sqrt (A);
    cd h;

    switch (type)
    {
        case FilterType::HighPass12: h = secondOrderHP (s, q); break;
        case FilterType::HighPass24: h = secondOrderHP (s, 0.54119610) * secondOrderHP (s, 1.30656296); break;
        case FilterType::LowPass12:  h = secondOrderLP (s, q); break;
        case FilterType::BandPass:   h = (s / q) / (s * s + s / q + 1.0); break;
        case FilterType::Bell:       h = (s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0); break;
        case FilterType::LowShelf:   h = A * (s * s + s * (sqA / q) + A) / (A * s * s + s * (sqA / q) + 1.0); break;
        case FilterType::HighShelf:  h = A * (A * s * s + s * (sqA / q) + 1.0) / (s * s + s * (sqA / q) + A); break;
    }
    return std::abs (h);
}

inline double magnitudeDb (FilterType type, double fc, double q, double gainDb, double f, double fs)
{
    return gainToDb (magnitude (type, fc, q, gainDb, f, fs));
}
} // namespace va
