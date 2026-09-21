#pragma once

#include <limits>

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"

namespace drumseq
{

class DynamicSequencerPad final : public juce::Component
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

    // Polled from the message thread (editor timer). Repaints only when the
    // atomic velocity or playhead state actually changed.
    void refreshFromEngine ();

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void setVelocity01 (float velocity01);

    PluginAudioProcessor& processor;
    const int laneIndex;
    const int stepIndex;
    juce::String velocityParameterID;
    juce::RangedAudioParameter* velocityParameter = nullptr;
    float dragOriginVelocity127 = 0.0f;
    float lastPaintedVelocity = -1.0f;
    int   lastPaintedStep     = std::numeric_limits<int>::min ();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicSequencerPad)
};

}