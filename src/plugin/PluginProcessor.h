// Vokal Assistent - audio processor
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "Params.h"
#include "Analyzer.h"

class VokalAssistentProcessor : public juce::AudioProcessor,
                                public juce::ChangeBroadcaster,
                                private juce::AsyncUpdater
{
public:
    VokalAssistentProcessor();
    ~VokalAssistentProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- analysis (message thread only)
    enum class AnalysisState { Idle, Recording, Analysing };
    void startAnalysis();
    void stopAnalysis();          // stops recording and analyses in the background
    void cancelAnalysis();
    AnalysisState getAnalysisState() const noexcept { return analysisState.load(); }
    double getRecordedSeconds() const noexcept;
    static constexpr double maxRecordSeconds = 180.0;
    bool recordingIsFull() const noexcept;

    std::shared_ptr<const va::AnalysisData> getAnalysis() const;
    va::Suggestion getSuggestion() const;   // uses the current profile / amount parameters
    void applySuggestion();
    juce::String getLastError() const { return lastError; }

    // ---- for the editor
    juce::AudioProcessorValueTreeState apvts;
    va::EqSettings readEqSettings() const;          // the target settings from parameters
    float getDeEssReduction() const noexcept { return deEssReduction.load(); }
    double getCurrentSampleRate() const noexcept { return currentSampleRate; }

    // Post-processing audio for the spectrum display (single reader: the editor).
    juce::AbstractFifo spectrumFifo { 1 << 15 };
    std::vector<float> spectrumBuffer = std::vector<float> (1 << 15);

private:
    template <typename Sample>
    void process (juce::AudioBuffer<Sample>& buffer);
    va::DeEssSettings readDeEssSettings() const;
    void smoothTowards (const va::EqSettings& target, double alpha);
    void handleAsyncUpdate() override;

    double currentSampleRate = 48000.0;
    std::vector<va::VocalEqChannel> eqChannels;
    va::DeEsser deEsser;
    va::EqSettings currentEq = va::EqSettings::defaults();
    bool firstBlock = true;
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> outGainSmoothed;
    std::atomic<float> deEssReduction { 0.0f };

    // Recording for analysis. The audio thread only try-locks.
    juce::SpinLock recordLock;
    std::vector<float> recordBuffer;
    std::atomic<size_t> recordPos { 0 };
    std::atomic<bool> recording { false };
    double recordSampleRate = 0.0;
    std::atomic<AnalysisState> analysisState { AnalysisState::Idle };

    mutable std::mutex analysisMutex;
    std::shared_ptr<const va::AnalysisData> analysis;
    std::shared_ptr<va::AnalysisData> pendingResult;
    juce::String lastError;

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads (1) };   // declared last: destroyed (joined) first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VokalAssistentProcessor)
};
