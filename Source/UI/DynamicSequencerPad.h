#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"

namespace drumseq
{

class DynamicSequencerPad final : public juce::Component,
                                  public juce::AudioProcessorValueTreeState::Listener
{
public:
    enum
    {
        kBaselineVelocity = 100
    };

    static constexpr float kDragVelocityPerPadHeight = 100.0f;

    DynamicSequencerPad (PluginAudioProcessor& processor, int laneIndex, int stepIndex);
    ~DynamicSequencerPad () override;

    int getLaneIndex () const noexcept { return laneIndex; }
    int getStepIndex () const noexcept { return stepIndex; }

    float getVelocity127 () const;
    void setVelocity127 (float velocity);

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    void parameterChanged (const juce::String&, float) override;

private:
    void setVelocity01 (float velocity01);

    PluginAudioProcessor& processor;
    const int laneIndex;
    const int stepIndex;
    juce::String velocityParameterID;
    juce::RangedAudioParameter* velocityParameter = nullptr;
    float dragOriginVelocity127 = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicSequencerPad)
};

}