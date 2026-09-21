#pragma once

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DesignSystem/SequencerDesignSystem.h"
#include "PluginProcessor.h"
#include "UI/DynamicSequencerPad.h"

namespace drumseq
{

class PluginAudioEditor final : public juce::AudioProcessorEditor,
                                public juce::Timer
{
public:
    static constexpr int kStepsAcross = 8;
    static constexpr int kStepsDown   = 4;
    static constexpr int kPadsPerLane = kStepsAcross * kStepsDown;

    explicit PluginAudioEditor (PluginAudioProcessor&);
    ~PluginAudioEditor () override;

    void paint (juce::Graphics&) override;
    void resized () override;
    void timerCallback () override;

private:
    class TrackHeaderRow;
    class MidiLearnButton;

    void showLane (int laneIndex);
    void updateTrackHeaders ();
    void updatePlayhead ();

    PluginAudioProcessor& processor;
    SequencerDesignSystem lookAndFeel;

    juce::Label globalTitle;

    juce::Label swingLabel;
    juce::Slider swingSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> swingAttachment;

    juce::Label bankLabel;
    std::array<std::unique_ptr<juce::ToggleButton>, PluginAudioProcessor::kNumNoteBanks> bankButtons;

    std::array<std::unique_ptr<TrackHeaderRow>, PluginAudioProcessor::kNumLanes> trackHeaders;

    std::array<std::array<std::unique_ptr<DynamicSequencerPad>, kPadsPerLane>,
               PluginAudioProcessor::kNumLanes> padGrid;

    int selectedLane     = 0;
    int lastPlayheadStep = -2;
    int sidebarDividerX  = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginAudioEditor)
};

}