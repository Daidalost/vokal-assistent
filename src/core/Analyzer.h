// Vokal Assistent - offline vocal analysis and EQ / de-ess suggestion engine.
// No JUCE dependency: used by the plugin, the command line tool and the tests.
#pragma once

#include "VocalEQ.h"
#include "DeEsser.h"
#include <string>
#include <vector>

namespace va
{
enum class VoiceType { Male = 0, Female = 1 };
enum class ProfileChoice { Auto = 0, Male = 1, Female = 2 };

struct Resonance
{
    double freq = 0.0;
    double excessDb = 0.0;   // how far the peak stands out of its surroundings
    double q = 4.0;
    bool uncertain = false;  // could be a held note's harmonic rather than a resonance
};

struct AnalysisData
{
    bool valid = false;
    std::string error;

    double sampleRate = 0.0;
    double seconds = 0.0;          // length of the analysed audio
    double activeSeconds = 0.0;    // time with voice present
    double voicedSeconds = 0.0;    // time with a clear pitch
    double sibilantSeconds = 0.0;  // time with S-like sounds
    double peakDb = -150.0;        // sample peak
    double activeRmsDb = -150.0;   // average level while singing
    double noiseFloorDb = -150.0;  // level in the pauses (if any)
    bool hasPauses = false;

    // Pitch (Hz). 0 when not measurable.
    double f0Median = 0.0, f0Low = 0.0, f0High = 0.0;
    VoiceType detectedVoice = VoiceType::Male;
    double voiceConfidence = 0.0;  // 0..1

    // Long-term spectrum of the tonal (vowel) parts, 1/3 octave band power in
    // dB on the 1/12 octave grid below (not normalised).
    std::vector<double> gridHz;
    std::vector<double> tonalDb;
    std::vector<double> sibilantDb;  // same, for the S frames (density, dB)

    std::vector<Resonance> resonances;
    double rumbleDb = -150.0;        // energy below the voice, relative to total

    // S-sound statistics (detector levels in dBFS, raw input).
    int numSibilantFrames = 0;
    double sibFreq = 6500.0;
    double sibDetP50 = -150.0, sibDetP90 = -150.0;
    double vowelDetP99 = -150.0, vowelPeakP90 = -150.0;
};

struct ProblemArea
{
    std::string name;
    std::string range;
    double deviationDb = 0.0;  // + means more energy than the target profile
    std::string verdict;
};

struct Suggestion
{
    bool valid = false;
    VoiceType profile = VoiceType::Male;
    double amount = 1.0;
    EqSettings eq = EqSettings::defaults();
    DeEssSettings deess;
    std::vector<double> targetDb;           // target curve on the grid (aligned to measured)
    std::vector<double> measuredDb;         // measured tonal curve on the grid
    std::vector<ProblemArea> areas;
    std::vector<std::string> notes;
};

// Analyse mono audio. Safe to call on a background thread.
AnalysisData analyze (const float* samples, size_t numSamples, double sampleRate);

// Build the EQ and de-esser suggestion. amount scales the EQ gains (1 = 100 %).
// With a reference analysis, its tonal curve is used as the target instead of
// the built-in male/female profile ("sound like this vocal").
Suggestion suggest (const AnalysisData& data, ProfileChoice profile, double amount = 1.0,
                    const AnalysisData* reference = nullptr);

// Plain text report (English) for display.
std::string formatReport (const AnalysisData& data, const Suggestion& s);

// Target curve (relative 1/3 octave band level, dB) for a voice type at f.
double targetCurveDb (VoiceType v, double f0, double f);

// Serialisation of AnalysisData (so a saved Cubase project keeps its analysis).
std::string serialise (const AnalysisData& d);
bool deserialise (const std::string& text, AnalysisData& d);
} // namespace va
