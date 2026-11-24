// Vokal Assistent - EQ curve, live spectrum and analysis overlay
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"

class EqDisplay : public juce::Component
{
public:
    explicit EqDisplay (VokalAssistentProcessor& p);

    void paint (juce::Graphics&) override;
    void resized() override {}

    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseExit (const juce::MouseEvent&) override;

    // Called from the editor's timer (message thread).
    void pullSpectrum();
    void setSuggestion (const va::Suggestion& s) { suggestion = s; repaint(); }
    void setShowAnalysis (bool b) { showAnalysis = b; repaint(); }

private:
    float xForFreq (double f) const;
    double freqForX (float x) const;
    float yForDb (double db) const;
    double dbForY (float y) const;
    juce::Point<float> handlePos (int band, const va::EqSettings& s) const;
    int hitBand (juce::Point<float> p) const;
    void setParam (const juce::String& id, float value);
    juce::Rectangle<float> plotArea() const;

    VokalAssistentProcessor& proc;
    va::Suggestion suggestion;
    bool showAnalysis = true;
    int dragBand = -1, hoverBand = -1;

    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> ring = std::vector<float> ((size_t) fftSize, 0.0f);
    int ringPos = 0;
    std::vector<float> fftData = std::vector<float> ((size_t) fftSize * 2, 0.0f);
    std::vector<float> spectrumDb = std::vector<float> ((size_t) fftSize / 2 + 1, -120.0f);

    static constexpr double minHz = 20.0, maxHz = 20000.0, dbRange = 18.0;
};
