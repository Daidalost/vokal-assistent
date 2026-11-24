// Vokal Assistent - small radix-2 FFT used by the offline analyser.
#pragma once

#include <complex>
#include <vector>
#include <cmath>
#include <cassert>

namespace va
{
class FFT
{
public:
    explicit FFT (int size) : n (size), twiddles ((size_t) size / 2), rev ((size_t) size)
    {
        assert (size >= 2 && (size & (size - 1)) == 0);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        for (int i = 0; i < n / 2; ++i)
            twiddles[(size_t) i] = std::polar (1.0, -2.0 * 3.14159265358979323846 * i / n);
    }

    int size() const noexcept { return n; }

    void forward (std::vector<std::complex<double>>& x) const
    {
        for (int i = 0; i < n; ++i)
            if (rev[(size_t) i] > i) std::swap (x[(size_t) i], x[(size_t) rev[(size_t) i]]);

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len / 2, step = n / len;
            for (int start = 0; start < n; start += len)
                for (int k = 0; k < half; ++k)
                {
                    auto& a = x[(size_t) (start + k)];
                    auto& b = x[(size_t) (start + k + half)];
                    const auto t = twiddles[(size_t) (k * step)] * b;
                    b = a - t;
                    a = a + t;
                }
        }
    }

    // Power spectrum (|X|^2) of a real frame, bins 0..n/2.
    void powerSpectrum (const double* frame, std::vector<double>& out)
    {
        buf.assign ((size_t) n, {});
        for (int i = 0; i < n; ++i) buf[(size_t) i] = { frame[i], 0.0 };
        forward (buf);
        out.resize ((size_t) n / 2 + 1);
        for (int k = 0; k <= n / 2; ++k) out[(size_t) k] = std::norm (buf[(size_t) k]);
    }

private:
    int n;
    std::vector<std::complex<double>> twiddles;
    std::vector<int> rev;
    std::vector<std::complex<double>> buf;
};
} // namespace va
