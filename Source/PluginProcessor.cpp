#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace drumseq
{

PluginAudioProcessor::PluginAudioProcessor ()
    : juce::AudioProcessor (juce::AudioProcessor::BusesProperties ()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo (), false)
                                .withOutput ("Output", juce::AudioChannelSet::stereo (),  true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout ())
{
    initialiseCaches ();

    apvts.addParameterListener ("swing", this);

    if (auto* swing = apvts.getParameter ("swing"))
        setSwing (swing->getValue ());
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginAudioProcessor::createParameterLayout ()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Swing is declared 0..1 directly (straight = 0.0f, fully swung = 1.0f).
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        "swing", "Swing",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f)
            .withStringFromValueFunction ([] (float v)
                                          {
                                              return juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, v) * 100.0f)) + "%";
                                          }),
        0.0f));

    // Per-step velocity parameters are declared *normalized* 0..1 to prevent
    // DAW saturation; the 0..127 display string is supplied purely for humans.
    for (int lane = 0; lane < kNumLanes; ++lane)
        for (int step = 0; step < kAutomationStepCount; ++step)
        {
            const auto velocityRange = juce::NormalisableRange<float> (0.0f, 1.0f, 1.0f / 127.0f)
                                           .withStringFromValueFunction ([] (float v)
                                                                         {
                                                                             return juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, v) * 127.0f));
                                                                         });

            layout.add (std::make_unique<juce::AudioParameterFloat> (
                laneStepVelParameterID (lane, step),
                "Lane " + juce::String (lane + 1) + " Step " + juce::String (step + 1) + " Velocity",
                velocityRange,
                0.0f));
        }

    return layout;
}

juce::String PluginAudioProcessor::laneStepVelParameterID (int lane, int step)
{
    return juce::String::formatted ("lane_%d_step_%d_vel", lane, step);
}

juce::RangedAudioParameter* PluginAudioProcessor::getLaneStepVelParameter (int lane, int step)
{
    if (lane < 0 || lane >= kNumLanes || step < 0 || step >= kAutomationStepCount)
        return nullptr;

    return apvts.getParameter (laneStepVelParameterID (lane, step));
}

void PluginAudioProcessor::initialiseCaches ()
{
    resetPatternData ();

    for (int lane = 0; lane < kNumLanes; ++lane)
        lastSteps[lane] = -1;

    timelinePrimed     = false;
    wasPlayingLast     = false;
    lastSixteenthFired = 0;
    noteOffCount       = 0;
}

void PluginAudioProcessor::resetPatternData ()
{
    laneInLearnMode.store (-1, std::memory_order_relaxed);
    swingParamCache.store (0.0f, std::memory_order_relaxed);
    fallbackBpmCache.store (120.0, std::memory_order_relaxed);

    activeNoteBank.store (0, std::memory_order_relaxed);
    mpdLaneBank.store (0, std::memory_order_relaxed);

    for (int bank = 0; bank < kNumNoteBanks; ++bank)
        for (int lane = 0; lane < kNumLanes; ++lane)
            noteBankCaches[(size_t) bank][(size_t) lane].store (36 + lane + bank * kNoteBankSpacing,
                                                                std::memory_order_relaxed);

    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        loopLengthCaches[(size_t) lane].store (16, std::memory_order_relaxed);
        velocityScaleCaches[(size_t) lane].store (1.0f, std::memory_order_relaxed);
        currentStepCaches[(size_t) lane].store (-1, std::memory_order_relaxed);

        for (int step = 0; step < kMaxStepsPerLane; ++step)
            stepVelocityCaches[(size_t) lane][(size_t) step].store (0.0f, std::memory_order_relaxed);
    }

    for (auto& slot : noteOffQueue)
        slot.active = false;

    noteOffCount = 0;
}

void PluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate    = sampleRate;
    noteOffQueue.fill ({});

    // Pre-roll the scratch surface and (indirectly) the host's MIDI buffer so
    // dense note queues never allocate on the audio thread.
    scratchBuffer.ensureSize (8192u);

    if (auto* swing = apvts.getParameter ("swing"))
        setSwing (swing->getValue ());
}

void PluginAudioProcessor::releaseResources ()
{
}

void PluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear ();

    processHardwareController (midiMessages);

    maybeProcessMIDILearn (midiMessages);

    const int numSamples = buffer.getNumSamples ();

    bool isPlaying = false;
    double ppqStart = 0.0;
    double bpm      = fallbackBpmCache.load (std::memory_order_relaxed);
    double sampleRate = getSampleRate ();

    if (sampleRate <= 0.0)
        sampleRate = currentSampleRate;

    if (auto* playhead = getPlayHead ())
    {
        if (const auto position = playhead->getPosition ())
        {
            isPlaying = position->getIsPlaying ();

            if (const auto b = position->getBpm ())
                bpm = *b;

            if (const auto ppq = position->getPpqPosition ())
                ppqStart = *ppq;
        }
    }

    currentSampleRate = sampleRate;
    fallbackBpmCache.store (bpm, std::memory_order_relaxed);

    if (isPlaying)
    {
        processPendingNoteOffs (midiMessages, numSamples);
        scheduleLanes (midiMessages, numSamples, ppqStart, bpm, sampleRate);
    }
    else
    {
        stopAndFlush (midiMessages);
    }

    wasPlayingLast = isPlaying;
}

// ---------------------------------------------------------------------------
// Raw-byte MIDI parsing. metadata.getMessage() is deliberately avoided on the
// audio thread: the status/note/CC bytes are read straight off the event
// pointer so large SysEx blocks and handshake replies never get materialized.
// ---------------------------------------------------------------------------

void PluginAudioProcessor::maybeProcessMIDILearn (juce::MidiBuffer& midiMessages)
{
    const int laneToLearn = laneInLearnMode.load (std::memory_order_relaxed);

    if (laneToLearn < 0)
        return;

    for (const auto& metadata : midiMessages)
    {
        if (metadata.numBytes < 3)
            continue;

        const auto* data = metadata.data;

        if ((data[0] & 0xF0) != 0x90)            // note-on family
            continue;

        if ((data[0] & 0x0F) != (kBasicChannel - 1))   // MIDI channel 1 only
            continue;

        const int velocity = data[2];

        if (velocity == 0)
            continue;

        const int capturedNote = data[1];

        // Assign into the active note bank first: even if a concurrent UI
        // action steals learn mode, the hardware hit remains programmed.
        noteBankCaches[(size_t) activeNoteBank.load (std::memory_order_relaxed)][(size_t) laneToLearn].store (
            capturedNote, std::memory_order_relaxed);

        // compare_exchange_strong: only disarm if we still own learn mode; a
        // cross-thread re-arm mid-capture must not be clobbered to -1.
        int expected = laneToLearn;

        if (laneInLearnMode.compare_exchange_strong (expected, -1,
                                                     std::memory_order_relaxed,
                                                     std::memory_order_relaxed))
        {
            midiMessages.clear ();
            return;
        }
    }
}

void PluginAudioProcessor::processHardwareController (juce::MidiBuffer& midiMessages)
{
    const int laneBank = mpdLaneBank.load (std::memory_order_relaxed) != 0 ? 8 : 0;

    for (const auto& metadata : midiMessages)
    {
        if (metadata.numBytes < 3)
            continue;

        const auto* data = metadata.data;

        if ((data[0] & 0xF0) != 0xB0)                  // control change family
            continue;

        if ((data[0] & 0x0F) != (kBasicChannel - 1))   // MIDI channel 1 only
            continue;

        const int cc    = data[1];
        const int value = data[2];

        if (cc >= 12 && cc <= 19)
        {
            const int lane = (cc - 12) + laneBank;
            setVelocityScale (lane, value / 127.0f);
        }
        else if (cc >= 22 && cc <= 29)
        {
            const int lane = (cc - 22) + laneBank;
            setLoopLength (lane, value);
        }
        else if (cc >= 32 && cc <= 39)
        {
            const int lane = (cc - 32) + laneBank;

            if (getLaneInLearnMode () == lane)
                disarmMIDILearn ();
            else
                armMIDILearn (lane);
        }
    }
}

// ---------------------------------------------------------------------------
// Persistent note-off queue. Note-offs are never clamped to the current block:
// a lookahead entry carries its absolute sample delta and is decremented every
// block until the owning future block arrives.
// ---------------------------------------------------------------------------

void PluginAudioProcessor::processPendingNoteOffs (juce::MidiBuffer& midiMessages, int numSamples)
{
    if (noteOffCount == 0)
        return;

    const std::int64_t blockLength = (std::int64_t) juce::jmax (0, numSamples);

    int out = 0;

    for (int i = 0; i < noteOffCount; ++i)
    {
        auto& slot = noteOffQueue[(size_t) i];

        if (slot.remainingSamples >= blockLength)
        {
            slot.remainingSamples -= blockLength;

            if (out != i)
                noteOffQueue[(size_t) out] = slot;

            ++out;
        }
        else
        {
            const int samplePos = juce::jlimit (0, numSamples, (int) slot.remainingSamples);

            midiMessages.addEvent (juce::MidiMessage::noteOff (kBasicChannel, slot.note, juce::uint8 (0)),
                                   samplePos);
        }
    }

    noteOffCount = out;

    for (int i = out; i < kNoteOffQueueCapacity; ++i)
        noteOffQueue[(size_t) i].active = false;
}

void PluginAudioProcessor::queueNoteOff (juce::MidiBuffer& midiMessages, int numSamples,
                                        int note, std::int64_t absoluteSample)
{
    if (noteOffCount < kNoteOffQueueCapacity)
    {
        noteOffQueue[(size_t) noteOffCount] = { absoluteSample, note, true };
        ++noteOffCount;
        return;
    }

    // Pool exhausted (512 simultaneous notes is pathological): flush the oldest
    // entry immediately instead of ever silently dropping a note-off.
    auto& oldest = noteOffQueue[(size_t) 0];
    midiMessages.addEvent (juce::MidiMessage::noteOff (kBasicChannel, oldest.note, juce::uint8 (0)),
                           juce::jlimit (0, juce::jmax (0, numSamples - 1), (int) oldest.remainingSamples));

    for (int i = 1; i < noteOffCount; ++i)
        noteOffQueue[(size_t) (i - 1)] = noteOffQueue[(size_t) i];

    --noteOffCount;
    queueNoteOff (midiMessages, numSamples, note, absoluteSample);
}

// ---------------------------------------------------------------------------
// Stateless absolute-sample window scheduler.
// ---------------------------------------------------------------------------

void PluginAudioProcessor::scheduleLanes (juce::MidiBuffer& midiMessages, int numSamples,
                                          double ppqStart, double bpm, double sampleRate)
{
    if (numSamples <= 0 || bpm <= 0.0 || sampleRate <= 0.0)
        return;

    const double samplesPerStep = (60.0 * sampleRate * 0.25) / bpm;
    const double blockSpan      = (double) numSamples / samplesPerStep;

    // Absolute 16th position at the head of this block.
    const double startStep = ppqStart * 4.0;

    if (! timelinePrimed)
    {
        timelinePrimed = true;

        for (int lane = 0; lane < kNumLanes; ++lane)
            lastSteps[lane] = -1;

        lastSixteenthFired = (int) std::floor (startStep) - 1;
    }

    // Window [start, end) of absolute 16th boundaries inside this block.
    // ceil-based math (with a tiny epsilon) removes the float equality check;
    // exact boundaries are included exactly once.
    constexpr double kE  = 1e-9;
    const int startBoundary = (int) std::ceil (startStep - kE);
    const int endBoundaryExclusive = (int) std::ceil (startStep + blockSpan - kE);

    if (startBoundary - lastSixteenthFired > 1
        && (startBoundary < lastSixteenthFired
            || startBoundary - lastSixteenthFired > kMaxStepsPerLane * 2))
    {
        for (int lane = 0; lane < kNumLanes; ++lane)
            lastSteps[lane] = -1;

        lastSixteenthFired = startBoundary - 1;
    }

    for (int absolute16 = startBoundary; absolute16 < endBoundaryExclusive; ++absolute16)
    {
        const int baseSample = (int) std::floor ((absolute16 - startStep) * samplesPerStep + 0.5);

        for (int lane = 0; lane < kNumLanes; ++lane)
            renderLaneHit (midiMessages, lane, absolute16, baseSample, samplesPerStep, numSamples);

        lastSixteenthFired = absolute16;
    }
}

void PluginAudioProcessor::renderLaneHit (juce::MidiBuffer& midiMessages, int lane,
                                          int absoluteSixteenth, int baseSample,
                                          double samplesPerStep, int numSamples)
{
    // Deduplicate per *absolute* 16th, not per loop step: a live loop-length
    // edit or a Loop Length of 1 re-fires on every boundary without skipping.
    if (absoluteSixteenth == lastSteps[lane])
        return;

    lastSteps[lane] = absoluteSixteenth;

    const int loopLength = std::max (1, loopLengthCaches[(size_t) lane].load (std::memory_order_relaxed));
    const int laneStep   = positiveMod (absoluteSixteenth, loopLength);

    currentStepCaches[(size_t) lane].store (laneStep, std::memory_order_relaxed);

    const float velocity01 = stepVelocityCaches[(size_t) lane][(size_t) laneStep].load (std::memory_order_relaxed);

    if (velocity01 <= 0.0f)
        return;

    // Note resolved at run-time from the atomic bank index; no 16-lane
    // propagation copy on the UI thread required.
    const int activeBank = activeNoteBank.load (std::memory_order_relaxed);
    const int midiNote   = noteBankCaches[(size_t) activeBank][(size_t) lane].load (std::memory_order_relaxed);

    if (midiNote < 0 || midiNote > 127)
        return;

    const float swing = juce::jlimit (0.0f, 1.0f, swingParamCache.load (std::memory_order_relaxed));

    // Global parity swing: shuffle locks to the absolute 16th grid (not the
    // lane's modulo), so odd loop lengths keep a steady off-beat groove.
    // swing == 0.0f yields a perfectly straight grid.
    const int swingSamples = positiveMod (absoluteSixteenth, 2) == 0
                                 ? 0
                                 : (int) std::lround (samplesPerStep * (double) swing * 0.5);

    const int samplePos    = juce::jlimit (0, numSamples, baseSample + swingSamples);
    const float scale      = juce::jlimit (0.0f, 1.0f,
                                           velocityScaleCaches[(size_t) lane].load (std::memory_order_relaxed));
    const int velocityMidi = juce::jlimit (1, 127, (int) std::lround (velocity01 * scale * 127.0f));

    midiMessages.addEvent (juce::MidiMessage::noteOn (kBasicChannel, midiNote, (juce::uint8) velocityMidi),
                           samplePos);

    const std::int64_t noteOffSamples = (std::int64_t) std::lround (currentSampleRate * 0.08);
    const std::int64_t noteOffSlot    = (std::int64_t) samplePos + noteOffSamples;

    if (noteOffSlot < numSamples)
    {
        midiMessages.addEvent (juce::MidiMessage::noteOff (kBasicChannel, midiNote, juce::uint8 (0)),
                               (int) noteOffSlot);
    }
    else
    {
        queueNoteOff (midiMessages, numSamples, midiNote, noteOffSlot);
    }
}

void PluginAudioProcessor::stopAndFlush (juce::MidiBuffer& midiMessages)
{
    if (wasPlayingLast)
    {
        midiMessages.addEvent (juce::MidiMessage::allNotesOff (kBasicChannel), 0);
        wasPlayingLast = false;

        for (int lane = 0; lane < kNumLanes; ++lane)
            currentStepCaches[(size_t) lane].store (-1, std::memory_order_relaxed);

        for (auto& slot : noteOffQueue)
            slot.active = false;

        noteOffCount = 0;
    }

    timelinePrimed = false;
}

int PluginAudioProcessor::positiveMod (int value, int modulo)
{
    const int divisor = std::max (1, modulo);
    const int result  = value % divisor;
    return result < 0 ? result + divisor : result;
}

void PluginAudioProcessor::parameterChanged (const juce::String& paramID, float newValue)
{
    if (paramID == "swing")
        setSwing (newValue);
}

void PluginAudioProcessor::setStepVelocity (int lane, int step, float value)
{
    if (lane < 0 || lane >= kNumLanes || step < 0 || step >= kMaxStepsPerLane)
        return;

    stepVelocityCaches[(size_t) lane][(size_t) step].store (juce::jlimit (0.0f, 1.0f, value),
                                                            std::memory_order_relaxed);
}

float PluginAudioProcessor::getStepVelocity (int lane, int step) const
{
    if (lane < 0 || lane >= kNumLanes || step < 0 || step >= kMaxStepsPerLane)
        return 0.0f;

    return stepVelocityCaches[(size_t) lane][(size_t) step].load (std::memory_order_relaxed);
}

void PluginAudioProcessor::setLoopLength (int lane, int length)
{
    if (lane < 0 || lane >= kNumLanes)
        return;

    loopLengthCaches[(size_t) lane].store (juce::jlimit (1, kMaxStepsPerLane, length),
                                           std::memory_order_relaxed);
}

int PluginAudioProcessor::getLoopLength (int lane) const
{
    if (lane < 0 || lane >= kNumLanes)
        return 1;

    return loopLengthCaches[(size_t) lane].load (std::memory_order_relaxed);
}

void PluginAudioProcessor::setTargetNote (int lane, int midiNote)
{
    setBankNote (activeNoteBank.load (std::memory_order_relaxed), lane, midiNote);
}

int PluginAudioProcessor::getTargetNote (int lane) const
{
    if (lane < 0 || lane >= kNumLanes)
        return 0;

    const int bank = activeNoteBank.load (std::memory_order_relaxed);

    return noteBankCaches[(size_t) bank][(size_t) lane].load (std::memory_order_relaxed);
}

void PluginAudioProcessor::setSwing (float value)
{
    swingParamCache.store (juce::jlimit (0.0f, 1.0f, value), std::memory_order_relaxed);
}

float PluginAudioProcessor::getSwing () const
{
    return swingParamCache.load (std::memory_order_relaxed);
}

void PluginAudioProcessor::setVelocityScale (int lane, float value)
{
    if (lane < 0 || lane >= kNumLanes)
        return;

    velocityScaleCaches[(size_t) lane].store (juce::jlimit (0.0f, 1.0f, value), std::memory_order_relaxed);
}

float PluginAudioProcessor::getVelocityScale (int lane) const
{
    if (lane < 0 || lane >= kNumLanes)
        return 1.0f;

    return velocityScaleCaches[(size_t) lane].load (std::memory_order_relaxed);
}

void PluginAudioProcessor::setActiveNoteBank (int bank)
{
    // Single atomic store; the real-time thread resolves the note at render
    // time from noteBankCaches[activeBank][lane]. No 16-lane loop copy.
    activeNoteBank.store (juce::jlimit (0, kNumNoteBanks - 1, bank), std::memory_order_relaxed);
}

int PluginAudioProcessor::getActiveNoteBank () const noexcept
{
    return activeNoteBank.load (std::memory_order_relaxed);
}

void PluginAudioProcessor::setBankNote (int bank, int lane, int midiNote)
{
    if (lane < 0 || lane >= kNumLanes)
        return;

    const int clampedBank = juce::jlimit (0, kNumNoteBanks - 1, bank);
    const int note        = juce::jlimit (0, 127, midiNote);

    noteBankCaches[(size_t) clampedBank][(size_t) lane].store (note, std::memory_order_relaxed);
}

int PluginAudioProcessor::getBankNote (int bank, int lane) const
{
    if (lane < 0 || lane >= kNumLanes || bank < 0 || bank >= kNumNoteBanks)
        return 0;

    return noteBankCaches[(size_t) bank][(size_t) lane].load (std::memory_order_relaxed);
}

int PluginAudioProcessor::getCurrentStep (int lane) const noexcept
{
    if (lane < 0 || lane >= kNumLanes)
        return -1;

    return currentStepCaches[(size_t) lane].load (std::memory_order_relaxed);
}

void PluginAudioProcessor::armMIDILearn (int lane)
{
    if (lane < 0 || lane >= kNumLanes)
        return;

    laneInLearnMode.store (lane, std::memory_order_relaxed);
}

void PluginAudioProcessor::disarmMIDILearn ()
{
    laneInLearnMode.store (-1, std::memory_order_relaxed);
}

int PluginAudioProcessor::getLaneInLearnMode () const noexcept
{
    return laneInLearnMode.load (std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Program abstraction (single default program).
// ---------------------------------------------------------------------------

int PluginAudioProcessor::getNumPrograms ()
{
    return 1;
}

int PluginAudioProcessor::getCurrentProgram ()
{
    return 0;
}

void PluginAudioProcessor::setCurrentProgram (int /*index*/)
{
}

const juce::String PluginAudioProcessor::getProgramName (int /*index*/)
{
    return juce::String (JucePlugin_Name) + " Default";
}

void PluginAudioProcessor::changeProgramName (int /*index*/, const juce::String& /*newName*/)
{
}

juce::AudioProcessorEditor* PluginAudioProcessor::createEditor ()
{
    return new PluginAudioEditor (*this);
}

bool PluginAudioProcessor::hasEditor () const
{
    return true;
}

const juce::String PluginAudioProcessor::getName () const
{
    return JucePlugin_Name;
}

bool PluginAudioProcessor::acceptsMidi () const
{
    return true;
}

bool PluginAudioProcessor::producesMidi () const
{
    return true;
}

double PluginAudioProcessor::getTailLengthSeconds () const
{
    return 0.0;
}

void PluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState ();
    std::unique_ptr<juce::XmlElement> xml (state.createXml ());
    copyXmlToBinary (*xml, destData);
}

void PluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr)
        return;

    const auto newTree = juce::ValueTree::fromXml (*xml);

    if (newTree.isValid ())
        apvts.replaceState (newTree);
}

bool PluginAudioProcessor::isBusesLayoutSupported (const juce::AudioProcessor::BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet ();
    return output == juce::AudioChannelSet::stereo ()
        || output == juce::AudioChannelSet::mono ()
        || output.isDisabled ();
}

}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter ()
{
    return new drumseq::PluginAudioProcessor ();
}