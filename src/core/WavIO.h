// Vokal Assistent - minimal WAV reader/writer for the command line tool and tests.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace va
{
struct Audio
{
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels;
    size_t length() const { return channels.empty() ? 0 : channels[0].size(); }
    std::vector<float> mono() const
    {
        std::vector<float> m (length(), 0.0f);
        for (const auto& c : channels)
            for (size_t i = 0; i < m.size(); ++i) m[i] += c[i] / (float) channels.size();
        return m;
    }
};

inline bool readWav (const std::string& path, Audio& out, std::string& error)
{
    FILE* f = std::fopen (path.c_str(), "rb");
    if (! f) { error = "cannot open " + path; return false; }
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t r;
    while ((r = std::fread (buf, 1, sizeof buf, f)) > 0) d.insert (d.end(), buf, buf + r);
    std::fclose (f);

    auto u16 = [&] (size_t o) { return (uint32_t) d[o] | ((uint32_t) d[o + 1] << 8); };
    auto u32 = [&] (size_t o) { return u16 (o) | (u16 (o + 2) << 16); };
    if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0)
    { error = "not a WAV file"; return false; }

    int fmtTag = 0, chans = 0, bits = 0;
    size_t pos = 12, dataOff = 0, dataLen = 0;
    while (pos + 8 <= d.size())
    {
        const uint32_t len = u32 (pos + 4);
        if (std::memcmp (d.data() + pos, "fmt ", 4) == 0 && pos + 24 <= d.size())
        {
            fmtTag = (int) u16 (pos + 8);
            chans = (int) u16 (pos + 10);
            out.sampleRate = (double) u32 (pos + 12);
            bits = (int) u16 (pos + 22);
            if (fmtTag == 0xFFFE && len >= 40) fmtTag = (int) u16 (pos + 32);   // extensible
        }
        else if (std::memcmp (d.data() + pos, "data", 4) == 0)
        {
            dataOff = pos + 8;
            dataLen = std::min ((size_t) len, d.size() - dataOff);
            break;
        }
        pos += 8 + len + (len & 1);
    }
    if (! dataOff || chans <= 0) { error = "no audio data"; return false; }
    const int bps = bits / 8;
    if (! ((fmtTag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (fmtTag == 3 && bits == 32)))
    { error = "unsupported WAV format"; return false; }

    const size_t frames = dataLen / (size_t) (bps * chans);
    out.channels.assign ((size_t) chans, std::vector<float> (frames));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < chans; ++c)
        {
            const unsigned char* p = d.data() + dataOff + (i * (size_t) chans + (size_t) c) * (size_t) bps;
            float v = 0.0f;
            if (fmtTag == 3) std::memcpy (&v, p, 4);
            else if (bits == 16) v = (float) (int16_t) (p[0] | (p[1] << 8)) / 32768.0f;
            else if (bits == 24) v = (float) (((int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24)) >> 8) / 8388608.0f;
            else { int32_t s; std::memcpy (&s, p, 4); v = (float) ((double) s / 2147483648.0); }
            out.channels[(size_t) c][i] = v;
        }
    return true;
}

inline bool writeWav (const std::string& path, const Audio& a)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) return false;
    const uint32_t ch = (uint32_t) a.channels.size(), sr = (uint32_t) a.sampleRate, n = (uint32_t) a.length();
    const uint32_t dataLen = n * ch * 4;
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataLen); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (3); w16 ((uint16_t) ch); w32 (sr); w32 (sr * ch * 4); w16 ((uint16_t) (ch * 4)); w16 (32);
    std::fwrite ("data", 1, 4, f); w32 (dataLen);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = 0; c < ch; ++c) std::fwrite (&a.channels[c][i], 4, 1, f);
    std::fclose (f);
    return true;
}
} // namespace va
