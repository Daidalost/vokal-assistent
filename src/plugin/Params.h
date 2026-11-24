// Vokal Assistent - parameter definitions
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "VocalEQ.h"
#include "DeEsser.h"

namespace va::params
{
// Parameter IDs. Never rename these: Cubase projects store values by ID.
inline juce::String bandPrefix (int band)
{
    static const char* names[] = { "hpf", "ls", "b1", "b2", "b3", "b4", "hs" };
    return names[band];
}
inline juce::String on (int band)   { return bandPrefix (band) + "_on"; }
inline juce::String freq (int band) { return bandPrefix (band) + "_freq"; }
inline juce::String gain (int band) { return bandPrefix (band) + "_gain"; }
inline juce::String q (int band)    { return bandPrefix (band) + "_q"; }
inline const juce::String hpfSlope  = "hpf_slope";
inline const juce::String dsOn      = "ds_on";
inline const juce::String dsFreq    = "ds_freq";
inline const juce::String dsThresh  = "ds_thresh";
inline const juce::String dsRange   = "ds_range";
inline const juce::String dsMode    = "ds_mode";
inline const juce::String dsListen  = "ds_listen";
inline const juce::String outGain   = "out_gain";
inline const juce::String profile   = "profile";
inline const juce::String amount    = "amount";

inline bool hasGain (int band) { return band != kHpf; }
inline bool hasQ (int band) { return band >= kBell1 && band <= kBell4; }

inline juce::String bandName (int band)
{
    static const char* names[] = { "Low Cut", "Low Shelf", "Bell 1", "Bell 2", "Bell 3", "Bell 4", "High Shelf" };
    return names[band];
}

inline juce::NormalisableRange<float> freqRange (float lo, float hi)
{
    juce::NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (std::sqrt (lo * hi));   // logarithmic feel
    return r;
}

inline juce::String hzToText (float v, int)
{
    if (v < 999.5f) return juce::String (juce::roundToInt (v)) + " Hz";
    const bool tenK = v >= 9995.0f;
    return juce::String (v / 1000.0f, tenK ? 1 : 2) + "k";
}
inline float textToHz (const juce::String& t)
{
    const auto v = t.getFloatValue();
    return t.containsIgnoreCase ("k") ? v * 1000.0f : v;
}
inline juce::String dbToText (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    const auto defaults = EqSettings::defaults();

    auto hzAttr = AudioParameterFloatAttributes().withStringFromValueFunction (hzToText).withValueFromStringFunction (textToHz);
    auto dbAttr = AudioParameterFloatAttributes().withStringFromValueFunction (dbToText);
    auto qAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2); });
    int version = 1;

    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& d = defaults.bands[(size_t) b];
        const auto name = bandName (b);
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { on (b), version }, name + " On", d.enabled));

        const float lo = b == kHpf ? 20.0f : b == kLowShelf ? 40.0f : b == kHighShelf ? 1500.0f : 40.0f;
        const float hi = b == kHpf ? 400.0f : b == kLowShelf ? 1000.0f : b == kHighShelf ? 18000.0f : 16000.0f;
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { freq (b), version }, name + " Freq",
                                                           freqRange (lo, hi), (float) d.freq, hzAttr));
        if (hasGain (b))
            layout.add (std::make_unique<AudioParameterFloat> (ParameterID { gain (b), version }, name + " Gain",
                                                               NormalisableRange<float> (-15.0f, 15.0f, 0.01f), 0.0f, dbAttr));
        if (hasQ (b))
        {
            NormalisableRange<float> qr (0.3f, 10.0f);
            qr.setSkewForCentre (1.0f);
            layout.add (std::make_unique<AudioParameterFloat> (ParameterID { q (b), version }, name + " Q", qr, (float) d.q, qAttr));
        }
    }
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { hpfSlope, version }, "Low Cut Slope",
                                                        StringArray { "12 dB/oct", "24 dB/oct" }, 0));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { dsOn, version }, "De-esser On", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { dsFreq, version }, "De-esser Freq", freqRange (2000.0f, 14000.0f), 6500.0f, hzAttr));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { dsThresh, version }, "De-esser Threshold",
                                                       NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -30.0f, dbAttr));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { dsRange, version }, "De-esser Range",
                                                       NormalisableRange<float> (0.0f, 20.0f, 0.1f), 8.0f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (
                                                           [] (float v, int) { return String (v, 1) + " dB"; })));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { dsMode, version }, "De-esser Mode", StringArray { "Wide", "Focused" }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { dsListen, version }, "De-esser Listen", false));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { outGain, version }, "Output Gain",
                                                       NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f, dbAttr));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { profile, version }, "Voice Profile",
                                                        StringArray { "Auto", "Male", "Female" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { amount, version }, "Suggestion Amount",
                                                       NormalisableRange<float> (0.0f, 150.0f, 1.0f), 100.0f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (
                                                           [] (float v, int) { return String (roundToInt (v)) + " %"; })));
    return layout;
}
} // namespace va::params
