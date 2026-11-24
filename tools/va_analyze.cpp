// Command line front end to the analyser: prints the report and can render the
// suggested processing to a new WAV file for listening tests.
//   va_analyze input.wav [--profile auto|male|female] [--amount 1.0] [--render out.wav]
#include "../src/core/Analyzer.h"
#include "../src/core/WavIO.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace va;

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: va_analyze input.wav [--profile auto|male|female] [--amount 1.0] [--render out.wav] [--curves]\n");
        return 2;
    }
    ProfileChoice profile = ProfileChoice::Auto;
    double amount = 1.0;
    const char* render = nullptr;
    bool curves = false;
    for (int i = 2; i < argc; ++i)
    {
        if (! std::strcmp (argv[i], "--profile") && i + 1 < argc)
        {
            ++i;
            profile = ! std::strcmp (argv[i], "male") ? ProfileChoice::Male : ! std::strcmp (argv[i], "female") ? ProfileChoice::Female : ProfileChoice::Auto;
        }
        else if (! std::strcmp (argv[i], "--amount") && i + 1 < argc) amount = std::atof (argv[++i]);
        else if (! std::strcmp (argv[i], "--render") && i + 1 < argc) render = argv[++i];
        else if (! std::strcmp (argv[i], "--curves")) curves = true;
    }

    Audio in;
    std::string err;
    if (! readWav (argv[1], in, err))
    {
        std::printf ("error: %s\n", err.c_str());
        return 1;
    }
    const auto mono = in.mono();
    const auto data = analyze (mono.data(), mono.size(), in.sampleRate);
    const auto s = suggest (data, profile, amount);
    std::printf ("%s", formatReport (data, s).c_str());
    if (curves && s.valid)
        for (size_t i = 0; i < data.gridHz.size(); ++i)
            std::printf ("curve %8.1f measured %7.2f target %7.2f eq %6.2f\n", data.gridHz[i], s.measuredDb[i], s.targetDb[i],
                         eqResponseDb (s.eq, data.gridHz[i], in.sampleRate));

    if (render && s.valid)
    {
        Audio out = in;
        DeEsser ds;
        ds.prepare (in.sampleRate, (int) in.channels.size());
        ds.setSettings (s.deess);
        std::vector<float*> ptrs;
        for (auto& c : out.channels)
        {
            VocalEqChannel eq;
            eq.prepare (in.sampleRate);
            eq.setSettings (s.eq);
            for (auto& v : c) v = (float) eq.process (v);
            ptrs.push_back (c.data());
        }
        ds.process (ptrs.data(), (int) ptrs.size(), (int) out.length());
        if (! writeWav (render, out)) { std::printf ("error: cannot write %s\n", render); return 1; }
        std::printf ("\nRendered to %s\n", render);
    }
    return data.valid ? 0 : 1;
}
