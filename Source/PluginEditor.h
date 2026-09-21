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
    static constexpr int kSynthParamCount = 5;

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
    void refreshVisiblePads ();
    void rebuildSynthPanel (int laneIndex);

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

    // Parametric synth controls for the active lane. Attachments are rebuilt
    // when the selected lane changes; the 30 Hz timer keeps them in sync.
    juce::Label synthPanelTitle;
    std::array<juce::Label, kSynthParamCount> synthParamLabels;
    std::array<juce::Slider, kSynthParamCount> synthParamSliders;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,
               kSynthParamCount> synthParamAttachments;
    int synthPanelLane = -1;

    int selectedLane     = 0;
    int lastPlayheadStep = -2;
    int sidebarDividerX  = 0;

    // Set once the bankButtons/trackHeaders/padGrid child pools are fully
    // allocated. setResizeLimits()/setSize() inside the constructor dispatch a
    // synchronous resized() before those pools exist, so resized() must not
    // dereference them until this is true.
    bool uiInitialized = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginAudioEditor)
};

}