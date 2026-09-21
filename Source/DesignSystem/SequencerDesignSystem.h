#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace drumseq
{

class SequencerDesignSystem : public juce::LookAndFeel_V4
{
public:
    struct Palette
    {
        const juce::Colour background   { 0xff121214 };
        const juce::Colour padOff       { 0xff1e1e22 };
        const juce::Colour padOn        { 0xff00ff66 };
        const juce::Colour midiLearn    { 0xffff3366 };
        const juce::Colour text         { 0xffa0a0a5 };

        juce::Colour onPadTopColour (float velocity01) const noexcept
        {
            return padOn.withAlpha (juce::jmap (juce::jlimit (0.0f, 1.0f, velocity01), 0.35f, 1.00f));
        }

        juce::Colour onPadBottomColour (float velocity01) const noexcept
        {
            return padOn.withAlpha (juce::jmap (juce::jlimit (0.0f, 1.0f, velocity01), 0.10f, 0.55f));
        }

        juce::Colour onPadColour (float velocity01) const noexcept
        {
            return padOn.withAlpha (juce::jmap (juce::jlimit (0.0f, 1.0f, velocity01), 0.22f, 1.00f));
        }

        juce::Colour gridLineColour () const noexcept
        {
            return text.withAlpha (0.30f);
        }
    };

    inline static const Palette palette {};

    static constexpr float kPadCornerRadius = 4.0f;

    SequencerDesignSystem ();
    ~SequencerDesignSystem () override;

    static SequencerDesignSystem& getDefault ();

    static juce::Font sequenceNumberFont ();
    static juce::Font laneLabelFont ();
    static juce::Font macroFont ();

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawIncDecButtons (juce::Graphics&, juce::Button&,
                            bool isMouseOverButton, bool isButtonDown) override;

protected:
    virtual float        getPadVelocity (const juce::Button&) const;
    virtual bool         isMIDILearnActive (const juce::Button&) const;
    virtual juce::String getPadLabelText (const juce::Button&) const;

private:
    static juce::Path makePadPath (const juce::Rectangle<float>&, float radius, int connectedEdgeFlags);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerDesignSystem)
};

}