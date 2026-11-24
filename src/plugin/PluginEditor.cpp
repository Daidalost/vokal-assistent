// Vokal Assistent - editor
#include "PluginEditor.h"

using namespace va;

// ------------------------------------------------------------------ Knob
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& labelText, juce::Colour colour)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    slider.setColour (juce::Slider::rotarySliderFillColourId, colour);
    addAndMakeVisible (slider);
    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (11.0f));
    label.setColour (juce::Label::textColourId, theme::textDim());
    addAndMakeVisible (label);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (14));
    slider.setBounds (r);
}

// ------------------------------------------------------------------ meter
void GainReductionMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::bg());
    g.fillRoundedRectangle (r, 3.0f);
    const float frac = juce::jlimit (0.0f, 1.0f, value / 20.0f);
    g.setColour (theme::deess());
    g.fillRoundedRectangle (r.withHeight (r.getHeight() * frac), 3.0f);
    g.setColour (theme::textDim());
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("GR", r.removeFromBottom (14.0f), juce::Justification::centred);
}

// ------------------------------------------------------------------ editor
VokalAssistentEditor::VokalAssistentEditor (VokalAssistentProcessor& p)
    : AudioProcessorEditor (&p),
      proc (p),
      display (p),
      dsFreq (p.apvts, params::dsFreq, "Frequency", theme::deess()),
      dsThresh (p.apvts, params::dsThresh, "Threshold", theme::deess()),
      dsRange (p.apvts, params::dsRange, "Range", theme::deess()),
      outGain (p.apvts, params::outGain, "Output"),
      amount (p.apvts, params::amount, "Amount", theme::targetCol())
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (display);

    for (int b = 0; b < kNumBands; ++b)
    {
        auto& s = bands[(size_t) b];
        const auto col = theme::bandColour (b);
        s.onButton.setButtonText (juce::String (b + 1) + "  " + params::bandName (b));
        s.onButton.setColour (juce::ToggleButton::tickColourId, col);
        addAndMakeVisible (s.onButton);
        s.onAttachment = std::make_unique<ButtonAttachment> (p.apvts, params::on (b), s.onButton);
        s.freq = std::make_unique<Knob> (p.apvts, params::freq (b), "Freq", col);
        addAndMakeVisible (*s.freq);
        if (params::hasGain (b))
        {
            s.gain = std::make_unique<Knob> (p.apvts, params::gain (b), "Gain", col);
            addAndMakeVisible (*s.gain);
        }
        if (params::hasQ (b))
        {
            s.q = std::make_unique<Knob> (p.apvts, params::q (b), "Q", col);
            addAndMakeVisible (*s.q);
        }
        if (b == kHpf)
        {
            s.slope.addItemList ({ "12 dB/oct", "24 dB/oct" }, 1);
            addAndMakeVisible (s.slope);
            s.slopeAttachment = std::make_unique<ComboAttachment> (p.apvts, params::hpfSlope, s.slope);
        }
    }

    dsOn.setColour (juce::ToggleButton::tickColourId, theme::deess());
    dsListen.setColour (juce::ToggleButton::tickColourId, theme::deess());
    dsListen.setTooltip ("Hear only what the de-esser removes");
    for (auto* c : std::initializer_list<juce::Component*> { &dsOn, &dsListen, &dsFreq, &dsThresh, &dsRange, &outGain, &dsMode, &grMeter })
        addAndMakeVisible (c);
    dsOnAtt = std::make_unique<ButtonAttachment> (p.apvts, params::dsOn, dsOn);
    dsListenAtt = std::make_unique<ButtonAttachment> (p.apvts, params::dsListen, dsListen);
    dsMode.addItemList ({ "Wide", "Focused" }, 1);
    dsModeAtt = std::make_unique<ComboAttachment> (p.apvts, params::dsMode, dsMode);

    // Assistant
    analyzeButton.onClick = [this]
    {
        switch (proc.getAnalysisState())
        {
            case VokalAssistentProcessor::AnalysisState::Idle: proc.startAnalysis(); break;
            case VokalAssistentProcessor::AnalysisState::Recording: proc.stopAnalysis(); break;
            default: break;
        }
        updateAnalyzeButton();
    };
    applyButton.onClick = [this] { proc.applySuggestion(); };
    statusLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setColour (juce::Label::textColourId, theme::textDim());
    statusLabel.setJustificationType (juce::Justification::topLeft);
    profileLabel.setFont (juce::FontOptions (12.0f));
    profileBox.addItemList ({ "Auto", "Male", "Female" }, 1);
    profileAtt = std::make_unique<ComboAttachment> (p.apvts, params::profile, profileBox);
    showAnalysis.setToggleState (true, juce::dontSendNotification);
    showAnalysis.onClick = [this] { display.setShowAnalysis (showAnalysis.getToggleState()); };
    report.setMultiLine (true, false);
    report.setReadOnly (true);
    report.setScrollbarsShown (true);
    report.setCaretVisible (false);
    report.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::plain));
    for (auto* c : std::initializer_list<juce::Component*> { &analyzeButton, &applyButton, &statusLabel, &profileLabel, &profileBox,
                                                             &amount, &showAnalysis, &report })
        addAndMakeVisible (c);

    proc.addChangeListener (this);
    setResizable (true, true);
    setResizeLimits (1000, 640, 1800, 1100);
    setSize (1120, 700);
    refreshAnalysisUi();
    startTimerHz (30);
}

VokalAssistentEditor::~VokalAssistentEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void VokalAssistentEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg());
    g.setColour (theme::text());
    g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    g.drawText ("VOKAL ASSISTENT", 16, 8, 300, 26, juce::Justification::centredLeft);
    g.setColour (theme::textDim());
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Vocal EQ + De-esser with analysis", 200, 8, 300, 26, juce::Justification::centredLeft);

    for (auto r : { bandArea, deessArea, assistantArea })
    {
        g.setColour (theme::panel());
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
    }
    g.setColour (theme::textDim());
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("ASSISTANT", assistantArea.reduced (12, 8).removeFromTop (14), juce::Justification::centredLeft);

    // separators between band columns
    g.setColour (theme::grid());
    const int colW = bandArea.getWidth() / kNumBands;
    for (int b = 1; b < kNumBands; ++b)
        g.drawVerticalLine (bandArea.getX() + b * colW, (float) bandArea.getY() + 8.0f, (float) bandArea.getBottom() - 8.0f);
}

void VokalAssistentEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    r.removeFromTop (32);
    assistantArea = r.removeFromRight (320);
    r.removeFromRight (10);

    deessArea = r.removeFromBottom (118);
    r.removeFromBottom (8);
    bandArea = r.removeFromBottom (196);
    r.removeFromBottom (8);
    display.setBounds (r);

    // Band columns
    const int colW = bandArea.getWidth() / kNumBands;
    for (int b = 0; b < kNumBands; ++b)
    {
        auto col = juce::Rectangle<int> (bandArea.getX() + b * colW, bandArea.getY(), colW, bandArea.getHeight()).reduced (6, 6);
        auto& s = bands[(size_t) b];
        s.onButton.setBounds (col.removeFromTop (24));
        const int knobH = 84;
        auto top = col.removeFromTop (knobH);
        auto bottom = col.removeFromTop (knobH);
        if (s.gain != nullptr)
        {
            s.freq->setBounds (top.removeFromLeft (top.getWidth() / 2));
            s.gain->setBounds (top);
        }
        else
        {
            s.freq->setBounds (top);
        }
        if (s.q != nullptr)
            s.q->setBounds (bottom.withSizeKeepingCentre (bottom.getWidth() / 2, bottom.getHeight()));
        if (b == kHpf)
            s.slope.setBounds (bottom.removeFromTop (26).reduced (2, 0));
    }

    // De-esser row
    auto d = deessArea.reduced (10, 8);
    auto left = d.removeFromLeft (110);
    dsOn.setBounds (left.removeFromTop (26));
    dsMode.setBounds (left.removeFromTop (26).reduced (0, 2));
    left.removeFromTop (4);
    dsListen.setBounds (left.removeFromTop (26));
    grMeter.setBounds (d.removeFromLeft (26).reduced (0, 4));
    d.removeFromLeft (12);
    const int kw = 90;
    dsFreq.setBounds (d.removeFromLeft (kw));
    dsThresh.setBounds (d.removeFromLeft (kw));
    dsRange.setBounds (d.removeFromLeft (kw));
    outGain.setBounds (d.removeFromRight (kw));

    // Assistant
    auto a = assistantArea.reduced (12, 8);
    a.removeFromTop (20);
    analyzeButton.setBounds (a.removeFromTop (34));
    a.removeFromTop (4);
    statusLabel.setBounds (a.removeFromTop (34));
    auto row = a.removeFromTop (28);
    profileLabel.setBounds (row.removeFromLeft (100));
    profileBox.setBounds (row);
    a.removeFromTop (4);
    auto row2 = a.removeFromTop (78);
    amount.setBounds (row2.removeFromLeft (90));
    row2.removeFromLeft (8);
    applyButton.setBounds (row2.withSizeKeepingCentre (row2.getWidth(), 34));
    showAnalysis.setBounds (a.removeFromTop (26));
    a.removeFromTop (4);
    report.setBounds (a);
}

void VokalAssistentEditor::updateAnalyzeButton()
{
    using S = VokalAssistentProcessor::AnalysisState;
    const auto st = proc.getAnalysisState();
    analyzeButton.setButtonText (st == S::Idle ? "Analyze" : st == S::Recording ? "Stop and analyze" : "Analysing...");
    analyzeButton.setEnabled (st != S::Analysing);
    analyzeButton.setColour (juce::TextButton::buttonColourId, st == S::Recording ? juce::Colour (0xffc62828) : theme::panelLight());

    juce::String status;
    if (st == S::Recording)
    {
        status << "Listening: " << juce::String (proc.getRecordedSeconds(), 0) << " s recorded. Play 30-60 s of the vocal, then press Stop.";
    }
    else if (st == S::Analysing)
    {
        status = "Analysing...";
    }
    else if (proc.getLastError().isNotEmpty())
    {
        status = proc.getLastError();
    }
    else if (proc.getAnalysis() == nullptr)
    {
        status = "Press Analyze and play the vocal track (solo is best).";
    }
    else
    {
        status = "Analysis ready. Choose profile and amount, then Apply.";
    }
    statusLabel.setText (status, juce::dontSendNotification);
}

void VokalAssistentEditor::refreshAnalysisUi()
{
    updateAnalyzeButton();
    const auto a = proc.getAnalysis();
    if (a == nullptr)
    {
        report.setText ("How to use:\n\n"
                        "1. Insert Vokal Assistent on the vocal track.\n"
                        "2. Press Analyze and play the song (the vocal\n"
                        "   should be audible; solo it if you like).\n"
                        "3. After 30-60 s press Stop and analyze.\n"
                        "4. Check the report and the curves, pick the\n"
                        "   profile and amount, then Apply suggestion.\n"
                        "5. Fine-tune by ear. Everything stays editable.\n\n"
                        "In the graph: drag the numbered points, use the\n"
                        "mouse wheel for Q and double-click to switch a\n"
                        "band on or off.");
        applyButton.setEnabled (false);
        display.setSuggestion ({});
        return;
    }
    const auto s = proc.getSuggestion();
    report.setText (formatReport (*a, s));
    applyButton.setEnabled (s.valid);
    display.setSuggestion (s);
}

void VokalAssistentEditor::changeListenerCallback (juce::ChangeBroadcaster*) { refreshAnalysisUi(); }

void VokalAssistentEditor::timerCallback()
{
    display.pullSpectrum();
    grMeter.setReduction (proc.getDeEssReduction());

    if (proc.getAnalysisState() == VokalAssistentProcessor::AnalysisState::Recording)
    {
        if (proc.recordingIsFull())
            proc.stopAnalysis();
        updateAnalyzeButton();
    }

    const float prof = proc.apvts.getRawParameterValue (params::profile)->load();
    const float amt = proc.apvts.getRawParameterValue (params::amount)->load();
    if (prof != lastProfile || amt != lastAmount)
    {
        lastProfile = prof;
        lastAmount = amt;
        refreshAnalysisUi();
    }
}
