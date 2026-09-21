#include "MidiDragExportComponent.h"

#include "../DesignSystem/SequencerDesignSystem.h"

namespace drumseq
{

namespace
{
constexpr int kTicksPerQuarter = 480;      // standard DAW PPQ resolution
constexpr int kTicksPer16th    = kTicksPerQuarter / 4;   // 120 ticks per step
constexpr int kNoteLengthTicks = kTicksPer16th / 2;      // 32nd-note staccato
constexpr int kDragStartThreshold = 8;     // px before the OS drag begins
}

MidiDragExportComponent::MidiDragExportComponent (PluginAudioProcessor& p)
    : processor (p)
{
    setTooltip ("Drag this out of the window to export the live 16-lane grid as a "
                "standard MIDI file (drop into an Ableton Live clip slot)");
}

MidiDragExportComponent::~MidiDragExportComponent ()
{
}

void MidiDragExportComponent::paint (juce::Graphics& g)
{
    const auto& pal = SequencerDesignSystem::palette;
    const auto area = getLocalBounds ().toFloat ();

    if (area.isEmpty ())
        return;

    const bool hover = isMouseOverOrDragging ();

    g.setColour (hover ? pal.accent.brighter (0.10f)
                       : pal.padOff.brighter (0.0f));
    g.fillRoundedRectangle (area, 4.0f);

    g.setColour (pal.accent.withAlpha (hover ? 0.95f : 0.55f));
    g.drawRoundedRectangle (area.reduced (1.5f), 4.0f, 1.4f);

    g.setFont (SequencerDesignSystem::sequenceNumberFont ());
    g.setColour (pal.text.withAlpha (hover ? 1.0f : 0.80f));
    g.drawText ("DRAG MIDI", area.toNearestInt (), juce::Justification::centred);

    // Small hardware-style dowel arrow hint under the label.
    g.setColour (pal.accent.withAlpha (0.75f));
    const auto centreX = juce::roundToInt (area.getCentreX ());
    const auto arrowY  = juce::roundToInt (area.getBottom () - 8.0f);
    g.drawHorizontalLine (arrowY - 2, (float) (centreX - 5), (float) (centreX + 5));
    g.drawHorizontalLine (arrowY - 1, (float) (centreX - 3), (float) (centreX + 3));
    g.drawHorizontalLine (arrowY,     (float) (centreX - 1), (float) (centreX + 1));
}

void MidiDragExportComponent::mouseDown (const juce::MouseEvent& event)
{
    dragStart  = event.position.toInt ();
    dragArmed  = false;
}

void MidiDragExportComponent::mouseDrag (const juce::MouseEvent& event)
{
    if (dragArmed)
        return;

    if (event.position.toInt ().getDistanceFrom (dragStart) < kDragStartThreshold)
        return;

    dragArmed = true;

    const juce::File midiFile = renderMidiFile ();

    if (midiFile.existsAsFile ())
        beginExternalDrag (midiFile);
}

juce::File MidiDragExportComponent::renderMidiFile ()
{
    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote (kTicksPerQuarter);

    // Track 0: tempo + time-signature map so hosts / DAWs read the clip cleanly.
    juce::MidiMessageSequence mapTrack;
    mapTrack.addEvent (juce::MidiMessage::tempoMetaEvent (500000), 0);   // 120 bpm
    mapTrack.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0);
    midiFile.addTrack (mapTrack);

    for (int lane = 0; lane < PluginAudioProcessor::kNumLanes; ++lane)
    {
        const int loopLength = juce::jmax (1, processor.getLoopLength (lane));
        const int midiNote   = juce::jlimit (0, 127, processor.getTargetNote (lane));
        const int channel    = (lane % 16) + 1;   // one lane per MIDI channel

        juce::MidiMessageSequence laneTrack;

        for (int step = 0; step < PluginAudioProcessor::kAutomationStepCount; ++step)
        {
            if (step >= loopLength)
                break;    // only emit the sounding window of the lane's loop

            const float velocity01 = processor.getStepVelocity (lane, step);

            if (velocity01 <= 0.0f)
                continue;

            const int eventTime = step * kTicksPer16th;
            const int velocityMidi = juce::jlimit (1, 127,
                juce::roundToInt (velocity01 * 127.0f));

            laneTrack.addEvent (juce::MidiMessage::noteOn (channel, midiNote,
                                                           (juce::uint8) velocityMidi),
                                eventTime);
            laneTrack.addEvent (juce::MidiMessage::noteOff (channel, midiNote,
                                                            (juce::uint8) 0),
                                eventTime + kNoteLengthTicks);
        }

        if (laneTrack.getNumEvents () > 0)
            midiFile.addTrack (laneTrack);
    }

    // Temp-dir serialization on the message thread (never the audio callback).
    const auto directory = juce::File::getSpecialLocation (juce::File::tempDirectory);
    const juce::File outFile = directory.getChildFile ("drumSeq_live_drag.mid");
    outFile.deleteFile ();

    juce::FileOutputStream stream (outFile);

    if (! stream.openedOk ())
        return {};

    if (! midiFile.writeTo (stream))
        return {};

    stream.flush ();

    return outFile;
}

void MidiDragExportComponent::beginExternalDrag (const juce::File& midiFile)
{
    if (midiFile == juce::File ())
        return;

    // JUCE represents an OS-level file drag as a StringArray of existing paths;
    // on macOS this materializes NSPasteboard file URLs (JUCE forwards the
    // drop through performExternalDragDropOfFiles), so the .mid can leave the
    // plugin window entirely (Ableton Live clip slot, Finder, QuickLook).
    const juce::StringArray fileList (midiFile.getFullPathName ());
    const juce::Image snapshot = createComponentSnapshot (getLocalBounds (), true);

    // Non-deprecated overload: explicit image scale, and macOS OS-level handoff
    // for StringArray-of-path descriptions.
    startDragging (fileList, this, juce::ScaledImage (snapshot), true);
}

}