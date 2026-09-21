#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"

namespace drumseq
{

// Hardware-style "DRAG MIDI" source. On drag start (message thread) the entire
// 16-lane x 32-step velocity grid is serialized into a standard multi-track
// MIDI file (.mid) under the system temp directory via juce::MidiFile, then the
// path is handed to JUCE's drag system so the user can drag the clip straight
// out of the window into an Ableton Live clip slot or a Finder drop zone.
// Real-time constraints: all file/disk work happens here on the message
// thread; the audio callback never touches the exporter. The component doubles
// as its own DragAndDropContainer (JUCE mixin) so the file drag can leave the
// plugin window entirely, and implements SettableTooltipClient for its hint.
class MidiDragExportComponent final : public juce::Component,
                                      public juce::DragAndDropContainer,
                                      public juce::SettableTooltipClient
{
public:
    explicit MidiDragExportComponent (PluginAudioProcessor& processor);
    ~MidiDragExportComponent () override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    juce::File renderMidiFile ();
    void beginExternalDrag (const juce::File&);

    PluginAudioProcessor& processor;
    juce::Point<int> dragStart;
    bool dragArmed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDragExportComponent)
};

}