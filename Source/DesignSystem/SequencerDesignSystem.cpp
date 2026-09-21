#include "SequencerDesignSystem.h"

#include <cmath>

namespace drumseq
{

SequencerDesignSystem::SequencerDesignSystem ()
{
    setDefaultSansSerifTypefaceName (juce::Font::getDefaultSansSerifFontName ());

    setColour (juce::Slider::rotarySliderFillColourId,      palette.padOn);
    setColour (juce::Slider::rotarySliderOutlineColourId,   palette.padOff);
    setColour (juce::Slider::thumbColourId,                 palette.text);
    setColour (juce::Slider::trackColourId,                 palette.padOff);
    setColour (juce::Slider::textBoxTextColourId,           palette.text);
    setColour (juce::Slider::textBoxOutlineColourId,        palette.padOff);
    setColour (juce::Slider::textBoxBackgroundColourId,     palette.background.brighter (0.04f));
    setColour (juce::Slider::textBoxHighlightColourId,      palette.padOn);
    setColour (juce::ToggleButton::textColourId,            palette.text);
    setColour (juce::ToggleButton::tickColourId,            palette.padOn);
    setColour (juce::TextEditor::textColourId,              palette.text);
    setColour (juce::TextEditor::backgroundColourId,        palette.background.brighter (0.05f));
    setColour (juce::TextEditor::outlineColourId,           palette.padOff);
    setColour (juce::TextEditor::highlightColourId,         palette.padOn.withAlpha (0.5f));
    setColour (juce::PopupMenu::backgroundColourId,         palette.background.brighter (0.08f));
    setColour (juce::PopupMenu::textColourId,               palette.text);
    setColour (juce::PopupMenu::headerTextColourId,         palette.padOn);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, palette.padOn.withAlpha (0.18f));
    setColour (juce::PopupMenu::highlightedTextColourId,    palette.padOn);
}

SequencerDesignSystem::~SequencerDesignSystem () = default;

SequencerDesignSystem& SequencerDesignSystem::getDefault ()
{
    static SequencerDesignSystem instance;
    return instance;
}

juce::Font SequencerDesignSystem::sequenceNumberFont ()
{
    return juce::Font (9.0f);
}

juce::Font SequencerDesignSystem::laneLabelFont ()
{
    return juce::Font (12.0f);
}

juce::Font SequencerDesignSystem::macroFont ()
{
    return juce::Font (16.0f, juce::Font::FontStyleFlags::bold);
}

juce::Path SequencerDesignSystem::makePadPath (const juce::Rectangle<float>& r, float radius,
                                               int connectedEdgeFlags)
{
    const bool roundedTopLeft     = (connectedEdgeFlags & juce::Button::ConnectedOnTop)    == 0
                                 && (connectedEdgeFlags & juce::Button::ConnectedOnLeft)  == 0;
    const bool roundedTopRight    = (connectedEdgeFlags & juce::Button::ConnectedOnTop)    == 0
                                 && (connectedEdgeFlags & juce::Button::ConnectedOnRight) == 0;
    const bool roundedBottomRight = (connectedEdgeFlags & juce::Button::ConnectedOnBottom) == 0
                                 && (connectedEdgeFlags & juce::Button::ConnectedOnRight) == 0;
    const bool roundedBottomLeft  = (connectedEdgeFlags & juce::Button::ConnectedOnBottom) == 0
                                 && (connectedEdgeFlags & juce::Button::ConnectedOnLeft)  == 0;

    const float x      = r.getX ();
    const float y      = r.getY ();
    const float right  = r.getRight ();
    const float bottom = r.getBottom ();
    const float rad    = juce::jmin (radius, r.getWidth () * 0.5f, r.getHeight () * 0.5f);

    juce::Path path;
    path.startNewSubPath (x + (roundedTopLeft ? rad : 0.0f), y);
    path.lineTo (right - (roundedTopRight ? rad : 0.0f), y);

    if (roundedTopRight)
        path.quadraticTo (right, y, right, y + rad);

    path.lineTo (right, bottom - (roundedBottomRight ? rad : 0.0f));

    if (roundedBottomRight)
        path.quadraticTo (right, bottom, right - rad, bottom);

    path.lineTo (x + (roundedBottomLeft ? rad : 0.0f), bottom);

    if (roundedBottomLeft)
        path.quadraticTo (x, bottom, x, bottom - rad);

    path.lineTo (x, y + (roundedTopLeft ? rad : 0.0f));

    if (roundedTopLeft)
        path.quadraticTo (x, y, x + rad, y);

    path.closeSubPath ();

    return path;
}

void SequencerDesignSystem::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                              bool, bool shouldDrawButtonAsDown)
{
    const auto area = button.getLocalBounds ().toFloat ();

    if (area.isEmpty ())
        return;

    const bool enabled = button.isEnabled ();
    const bool on      = button.getToggleState ();
    const bool hovered = enabled && button.isMouseOverOrDragging ();
    const bool down    = enabled && shouldDrawButtonAsDown;
    const bool learn   = enabled && isMIDILearnActive (button);

    const float velocity = (on && ! learn) ? getPadVelocity (button) : 0.0f;

    const juce::Path body = makePadPath (area, kPadCornerRadius, button.getConnectedEdgeFlags ());

    if (learn)
    {
        const juce::Path glow = makePadPath (area.expanded (1.6f), kPadCornerRadius + 1.0f,
                                             button.getConnectedEdgeFlags ());
        g.setColour (palette.midiLearn.withAlpha (0.28f));
        g.strokePath (glow, juce::PathStrokeType (2.5f));
    }

    if (on && ! learn)
    {
        const auto centre = area.getCentre ();
        const float halfHeight = area.getHeight () * 0.5f;

        juce::ColourGradient gradient (palette.onPadTopColour (velocity),
                                       centre.translated (0.0f, -halfHeight),
                                       palette.onPadBottomColour (velocity),
                                       centre.translated (0.0f, halfHeight),
                                       false);
        g.setGradientFill (gradient);
    }
    else
    {
        auto base = learn ? palette.midiLearn : palette.padOff;

        if (learn)
        {
            base = base.withAlpha (0.82f);
        }
        else if (hovered)
        {
            base = base.brighter (down ? 0.10f : 0.05f);
        }

        g.setColour (base.withAlpha (enabled ? 1.0f : 0.40f));
    }

    g.fillPath (body);

    auto borderColour = learn ? palette.midiLearn
                              : (on ? palette.padOn.withAlpha (0.95f)
                                    : palette.text.withAlpha (0.16f + (hovered ? 0.08f : 0.0f)));

    g.setColour (borderColour.withAlpha (enabled ? 1.0f : 0.40f));
    g.strokePath (body, juce::PathStrokeType (1.0f));

    if (const auto label = getPadLabelText (button); label.isNotEmpty ())
    {
        g.setFont (sequenceNumberFont ());
        g.setColour (palette.text.withAlpha (enabled ? 0.85f : 0.35f));

        if (isPadLabelCentred (button))
        {
            g.drawText (label,
                        area.withTrimmedTop (2.0f).withTrimmedBottom (2.0f).toNearestInt (),
                        juce::Justification::centred);
        }
        else
        {
            g.drawText (label,
                        area.withTrimmedTop (2.5f).withTrimmedLeft (3.0f).withTrimmedRight (1.0f).toNearestInt (),
                        juce::Justification::topLeft);
        }
    }
}

void SequencerDesignSystem::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                                              float sliderPosProportional, float rotaryStartAngle,
                                              float rotaryEndAngle, juce::Slider& slider)
{
    const juce::Rectangle<float> bounds ((float) x, (float) y, (float) w, (float) h);

    if (bounds.isEmpty ())
        return;

    const float thickness = juce::jmax (2.0f, (float) juce::jmin (w, h) * 0.07f);
    const float side      = juce::jmin (w, h) - thickness * 3.2f;

    auto dial = bounds.withSizeKeepingCentre (side, side);
    const auto centre = dial.getCentre ();
    const float outer = dial.getWidth () * 0.5f;

    const float angle = rotaryStartAngle
                        + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    const bool enabled = slider.isEnabled ();
    const bool hot     = enabled && slider.isMouseOverOrDragging ();

    juce::Path track;
    track.addArc (dial.getX (), dial.getY (), dial.getWidth (), dial.getHeight (),
                  rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (palette.text.withAlpha (enabled ? 0.20f : 0.10f));
    g.strokePath (track, juce::PathStrokeType (thickness));

    juce::Path valueArc;
    valueArc.addArc (dial.getX (), dial.getY (), dial.getWidth (), dial.getHeight (),
                     rotaryStartAngle, angle, true);
    g.setColour (palette.padOn.withAlpha (enabled ? (hot ? 1.0f : 0.85f) : 0.30f));
    g.strokePath (valueArc, juce::PathStrokeType (thickness));

    constexpr float halfPi = juce::MathConstants<float>::halfPi;

    const float cosine = std::cos (angle - halfPi);
    const float sine   = std::sin (angle - halfPi);

    const float inner = outer - thickness * 0.95f;
    const float outerEdge = outer - thickness * 0.20f;

    g.setColour (palette.text.withAlpha (enabled ? 0.95f : 0.40f));
    g.drawLine (centre.getX () + cosine * inner,   centre.getY () + sine * inner,
                centre.getX () + cosine * outerEdge, centre.getY () + sine * outerEdge,
                thickness * 0.55f);
}

class SequencerDesignSystem::IncDecButton final : public juce::Button
{
public:
    IncDecButton (const juce::String& name, bool shouldIncrement)
        : juce::Button (name),
          increment (shouldIncrement)
    {
    }

    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                      bool shouldDrawButtonAsDown) override
    {
        const auto area = getLocalBounds ().toFloat ();

        if (area.isEmpty ())
            return;

        const bool enabled = isEnabled ();

        auto fill = SequencerDesignSystem::palette.padOff.brighter (
            shouldDrawButtonAsHighlighted ? 0.12f : 0.0f);

        if (shouldDrawButtonAsDown)
            fill = SequencerDesignSystem::palette.padOn.withAlpha (0.20f);

        g.setColour (fill.withAlpha (enabled ? 1.0f : 0.45f));
        g.fillRoundedRectangle (area, SequencerDesignSystem::kPadCornerRadius);

        g.setColour (SequencerDesignSystem::palette.text.withAlpha (enabled ? 0.22f : 0.10f));
        g.drawRoundedRectangle (area, SequencerDesignSystem::kPadCornerRadius, 1.0f);

        const float cx = area.getCentreX ();
        const float cy = area.getCentreY ();
        const float sz = juce::jmin (area.getWidth (), area.getHeight ()) * 0.42f;

        juce::Path chevron;

        if (increment)
            chevron.addTriangle (cx - sz * 0.60f, cy + sz * 0.28f, cx + sz * 0.60f, cy + sz * 0.28f, cx, cy - sz * 0.40f);
        else
            chevron.addTriangle (cx - sz * 0.60f, cy - sz * 0.28f, cx + sz * 0.60f, cy - sz * 0.28f, cx, cy + sz * 0.40f);

        g.setColour (SequencerDesignSystem::palette.text.withAlpha (
            enabled ? (shouldDrawButtonAsDown ? 1.0f : 0.90f) : 0.35f));
        g.fillPath (chevron);
    }

private:
    const bool increment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IncDecButton)
};

juce::Button* SequencerDesignSystem::createSliderButton (juce::Slider& slider, bool isIncrement)
{
    return new IncDecButton (isIncrement ? "inc-button" : "dec-button", isIncrement);
}

float SequencerDesignSystem::getPadVelocity (const juce::Button& button) const
{
    const auto& props = button.getProperties ();
    const auto value  = props["velocity"];

    if (value.isVoid () || value.isUndefined ())
        return 1.0f;

    return juce::jlimit (0.0f, 1.0f, (float) value);
}

bool SequencerDesignSystem::isMIDILearnActive (const juce::Button& button) const
{
    const auto value = button.getProperties ()["midiLearn"];

    if (value.isVoid () || value.isUndefined ())
        return false;

    const auto text = value.toString ().trim ().toLowerCase ();
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

bool SequencerDesignSystem::isPadLabelCentred (const juce::Button& button) const
{
    const auto value = button.getProperties ()["padLabelCentre"];

    if (value.isVoid () || value.isUndefined ())
        return false;

    const auto text = value.toString ().trim ().toLowerCase ();
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

juce::String SequencerDesignSystem::getPadLabelText (const juce::Button& button) const
{
    const auto& props = button.getProperties ();

    if (props.contains ("padLabel"))
        return props["padLabel"].toString ();

    if (props.contains ("sequenceNumber"))
        return props["sequenceNumber"].toString ();

    return {};
}

}