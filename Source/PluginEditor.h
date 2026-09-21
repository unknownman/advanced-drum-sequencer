#pragma once

#include <array>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DesignSystem/SequencerDesignSystem.h"
#include "PluginProcessor.h"

namespace drumseq
{

class PluginAudioEditor final : public juce::AudioProcessorEditor
{
public:
    explicit PluginAudioEditor (PluginAudioProcessor&);
    ~PluginAudioEditor () override;

    void paint (juce::Graphics&) override;
    void resized () override;

private:
    PluginAudioProcessor& processor;
    SequencerDesignSystem lookAndFeel;

    juce::Label globalTitle;

    std::array<juce::ToggleButton, 8> pads;

    juce::Label swingLabel;
    juce::Slider swingSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> swingAttachment;

    juce::Label stepLabel;
    juce::Slider stepCounter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginAudioEditor)
};

}