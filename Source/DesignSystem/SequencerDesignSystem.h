#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace drumseq
{

class SequencerDesignSystem : public juce::LookAndFeel_V4
{
public:
    struct Palette
    {
        const juce::Colour background;
        const juce::Colour padOff;
        const juce::Colour padOn;
        const juce::Colour midiLearn;
        const juce::Colour text;
        const juce::Colour accent;

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

    // Values initialized here (not as default member initializers) to avoid an
    // AppleClang 17 bug: default member init + inline static aggregate caused
    // "'X' needed within definition of enclosing class ... outside of member functions".
    inline static const Palette palette {
        juce::Colour { 0xff121214 },
        juce::Colour { 0xff1e1e22 },
        juce::Colour { 0xff00ff66 },
        juce::Colour { 0xffff3366 },
        juce::Colour { 0xffa0a0a5 },
        juce::Colour { 0xff66ccff }
    };

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

    juce::Button* createSliderButton (juce::Slider& slider, bool isIncrement) override;

protected:
    virtual float        getPadVelocity (const juce::Button&) const;
    virtual bool         isMIDILearnActive (const juce::Button&) const;
    virtual bool         isPadLabelCentred (const juce::Button&) const;
    virtual juce::String getPadLabelText (const juce::Button&) const;

private:
    class IncDecButton;

    static juce::Path makePadPath (const juce::Rectangle<float>&, float radius, int connectedEdgeFlags);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerDesignSystem)
};

}