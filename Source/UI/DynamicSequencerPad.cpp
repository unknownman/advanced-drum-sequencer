#include "DynamicSequencerPad.h"

#include "../DesignSystem/SequencerDesignSystem.h"

namespace drumseq
{

DynamicSequencerPad::DynamicSequencerPad (PluginAudioProcessor& p, int lane, int step)
    : processor (p),
      laneIndex (lane),
      stepIndex (step)
{
    velocityParameterID = PluginAudioProcessor::laneStepVelParameterID (laneIndex, stepIndex);
    velocityParameter   = processor.getLaneStepVelParameter (laneIndex, stepIndex);

    if (velocityParameter != nullptr)
        processor.getAPVTS ().addParameterListener (velocityParameterID, this);
}

DynamicSequencerPad::~DynamicSequencerPad ()
{
    if (velocityParameter != nullptr)
        processor.getAPVTS ().removeParameterListener (velocityParameterID, this);
}

float DynamicSequencerPad::getVelocity127 () const
{
    return processor.getStepVelocity (laneIndex, stepIndex) * 127.0f;
}

void DynamicSequencerPad::setVelocity127 (float velocity)
{
    setVelocity01 (juce::jlimit (0.0f, 127.0f, velocity) / 127.0f);
}

void DynamicSequencerPad::setVelocity01 (float velocity01)
{
    const float clamped = juce::jlimit (0.0f, 1.0f, velocity01);

    processor.setStepVelocity (laneIndex, stepIndex, clamped);

    if (velocityParameter != nullptr)
        velocityParameter->setValueNotifyingHost (clamped);

    repaint ();
}

void DynamicSequencerPad::paint (juce::Graphics& g)
{
    const auto& pal = SequencerDesignSystem::palette;

    const auto area = getLocalBounds ().toFloat ();

    if (area.isEmpty ())
        return;

    const float velocity   = processor.getStepVelocity (laneIndex, stepIndex);
    const bool  active     = velocity > 0.0f;
    const bool  hovered    = isMouseOverOrDragging ();

    juce::Path body;
    body.addRoundedRectangle (area, SequencerDesignSystem::kPadCornerRadius);

    g.setColour (pal.padOff.brighter (hovered && ! active ? 0.05f : 0.0f));
    g.fillPath (body);

    if (active)
    {
        const auto  meter  = area.withTop (area.getBottom () - velocity * area.getHeight ());
        const float alpha  = juce::jmap (velocity, 0.0f, 1.0f, 0.4f, 1.0f);

        g.saveState ();
        g.reduceClipRegion (body);

        juce::ColourGradient gradient (pal.padOn.withAlpha (alpha),
                                       juce::Point<float> (meter.getCentreX (), meter.getY ()),
                                       pal.padOn.withAlpha (alpha * 0.85f),
                                       juce::Point<float> (meter.getCentreX (), meter.getBottom ()),
                                       false);
        g.setGradientFill (gradient);
        g.fillRect (meter);

        g.restoreState ();

        g.setColour (pal.padOn.withAlpha (hovered ? 1.0f : 0.90f));
        g.strokePath (body, juce::PathStrokeType (hovered ? 1.4f : 1.2f));
    }
    else
    {
        g.setColour (pal.text.withAlpha (hovered ? 0.28f : 0.16f));
        g.strokePath (body, juce::PathStrokeType (1.0f));
    }

    g.setFont (SequencerDesignSystem::sequenceNumberFont ());
    g.setColour (pal.text.withAlpha (active ? 0.90f : 0.65f));
    g.drawText (juce::String (stepIndex + 1), area.toNearestInt (), juce::Justification::centred);

    if (processor.getCurrentStep (laneIndex) == stepIndex)
    {
        g.setColour (pal.accent);
        g.fillEllipse (area.getCentreX () - 2.0f, area.getBottom () - 6.0f, 4.0f, 4.0f);
    }
}

void DynamicSequencerPad::mouseDown (const juce::MouseEvent&)
{
    const float velocity127 = getVelocity127 ();

    if (velocity127 <= 0.0f)
    {
        dragOriginVelocity127 = (float) kBaselineVelocity;
        setVelocity127 (dragOriginVelocity127);
    }
    else
    {
        dragOriginVelocity127 = velocity127;
        setVelocity127 (0.0f);
    }
}

void DynamicSequencerPad::mouseDrag (const juce::MouseEvent& event)
{
    if (getVelocity127 () <= 0.0f)
        return;

    const float dragRange = juce::jmax (1.0f, (float) getHeight ());
    const float dragY     = event.getOffsetFromDragStart ().y;

    const float newVelocity127 = juce::jlimit (1.0f, 127.0f,
                                               dragOriginVelocity127
                                                   - dragY * (kDragVelocityPerPadHeight / dragRange));

    setVelocity127 (newVelocity127);
}

void DynamicSequencerPad::parameterChanged (const juce::String& paramID, float newValue)
{
    if (paramID != velocityParameterID)
        return;

    processor.setStepVelocity (laneIndex, stepIndex, newValue);
    repaint ();
}

}