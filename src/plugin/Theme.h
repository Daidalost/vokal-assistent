// Vokal Assistent - colours and look and feel
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme
{
inline juce::Colour bg()         { return juce::Colour (0xff15171c); }
inline juce::Colour panel()      { return juce::Colour (0xff1d2027); }
inline juce::Colour panelLight() { return juce::Colour (0xff262a33); }
inline juce::Colour grid()       { return juce::Colour (0xff2b2f39); }
inline juce::Colour gridStrong() { return juce::Colour (0xff434957); }
inline juce::Colour text()       { return juce::Colour (0xffe6e8ee); }
inline juce::Colour textDim()    { return juce::Colour (0xff8a91a1); }
inline juce::Colour accent()     { return juce::Colour (0xff4fc3f7); }
inline juce::Colour spectrum()   { return juce::Colour (0xff5c6b8a); }
inline juce::Colour measured()   { return juce::Colour (0xffb39ddb); }
inline juce::Colour after()      { return juce::Colour (0xff81c784); }
inline juce::Colour targetCol()  { return juce::Colour (0xffffca28); }
inline juce::Colour deess()      { return juce::Colour (0xffff8a65); }

inline juce::Colour bandColour (int band)
{
    static const juce::uint32 c[] = { 0xff90a4ae, 0xffff8a65, 0xffffd54f, 0xffaed581, 0xff4dd0e1, 0xff9575cd, 0xfff06292 };
    return juce::Colour (c[band % 7]);
}

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        setColourScheme ({ bg(), panel(), panelLight(), gridStrong(), text(), accent(), juce::Colours::black, panelLight(), text() });
        setColour (juce::Slider::rotarySliderFillColourId, accent());
        setColour (juce::Slider::rotarySliderOutlineColourId, grid());
        setColour (juce::Slider::thumbColourId, text());
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxTextColourId, text());
        setColour (juce::TextButton::buttonColourId, panelLight());
        setColour (juce::TextButton::buttonOnColourId, accent().darker (0.3f));
        setColour (juce::ComboBox::backgroundColourId, panelLight());
        setColour (juce::ComboBox::outlineColourId, grid());
        setColour (juce::TextEditor::backgroundColourId, bg());
        setColour (juce::TextEditor::outlineColourId, grid());
        setColour (juce::TextEditor::textColourId, text());
        setColour (juce::Label::textColourId, text());
        setColour (juce::ToggleButton::tickColourId, accent());
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (juce::FontOptions (12.5f));
        return l;
    }

    juce::Font getLabelFont (juce::Label& l) override
    {
        if (dynamic_cast<juce::Slider*> (l.getParentComponent()) != nullptr)
            return juce::FontOptions (12.5f);
        return LookAndFeel_V4::getLabelFont (l);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float startAngle, float endAngle,
                           juce::Slider& s) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const auto centre = bounds.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const auto fill = s.findColour (juce::Slider::rotarySliderFillColourId);

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f, startAngle, endAngle, true);
        g.setColour (grid());
        g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Bipolar knobs (range around 0) fill from the centre.
        const auto range = s.getRange();
        const bool bipolar = range.getStart() < 0.0 && range.getEnd() > 0.0;
        const float from = bipolar ? startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle) : startAngle;
        juce::Path val;
        val.addCentredArc (centre.x, centre.y, radius - 2.0f, radius - 2.0f, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (s.isEnabled() ? fill : textDim());
        g.strokePath (val, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour (panelLight());
        g.fillEllipse (centre.x - radius * 0.62f, centre.y - radius * 0.62f, radius * 1.24f, radius * 1.24f);
        juce::Path needle;
        needle.addRoundedRectangle (-1.5f, -radius * 0.6f, 3.0f, radius * 0.35f, 1.5f);
        g.setColour (text());
        g.fillPath (needle, juce::AffineTransform::rotation (angle).translated (centre));
    }
};
} // namespace theme
