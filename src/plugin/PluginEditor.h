// Vokal Assistent - editor
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "EqDisplay.h"
#include "Theme.h"

class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& labelText,
          juce::Colour colour = theme::accent());
    void resized() override;
    juce::Slider slider;

private:
    juce::Label label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class GainReductionMeter : public juce::Component
{
public:
    void setReduction (float db) { if (std::abs (db - value) > 0.05f) { value = db; repaint(); } }
    void paint (juce::Graphics&) override;

private:
    float value = 0.0f;
};

class VokalAssistentEditor : public juce::AudioProcessorEditor,
                             private juce::Timer,
                             private juce::ChangeListener
{
public:
    explicit VokalAssistentEditor (VokalAssistentProcessor&);
    ~VokalAssistentEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshAnalysisUi();
    void updateAnalyzeButton();

    VokalAssistentProcessor& proc;
    theme::LookAndFeel lnf;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    EqDisplay display;

    // Bands
    struct BandStrip
    {
        juce::ToggleButton onButton;
        std::unique_ptr<ButtonAttachment> onAttachment;
        std::unique_ptr<Knob> freq, gain, q;
        juce::ComboBox slope;   // low cut only
        std::unique_ptr<ComboAttachment> slopeAttachment;
    };
    std::array<BandStrip, va::kNumBands> bands;

    // De-esser
    juce::ToggleButton dsOn { "De-esser" }, dsListen { "Listen" };
    std::unique_ptr<ButtonAttachment> dsOnAtt, dsListenAtt;
    Knob dsFreq, dsThresh, dsRange, outGain;
    juce::ComboBox dsMode;
    std::unique_ptr<ComboAttachment> dsModeAtt;
    GainReductionMeter grMeter;

    // Assistant
    juce::TextButton analyzeButton, applyButton { "Apply suggestion" };
    juce::Label statusLabel, profileLabel { {}, "Voice profile" };
    juce::ComboBox profileBox;
    std::unique_ptr<ComboAttachment> profileAtt;
    Knob amount;
    juce::ToggleButton showAnalysis { "Show analysis in graph" };
    juce::TextEditor report;

    float lastProfile = -1.0f, lastAmount = -1.0f;
    juce::Rectangle<int> bandArea, deessArea, assistantArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VokalAssistentEditor)
};
