#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace drumseq
{

namespace
{
constexpr double kTwoPi = 6.28318530717958647692;

// Structural tuning for the pre-allocated standalone drum models. Everything
// here is fixed at compile time so prepareToPlay() only computes per-sample
// constants (no heap, no disk, no look-up tables).
struct DrumSynthTuning
{
    double tailThreshold   = 1e-4;

    double kickBaseFreq    = 50.0;    // Hz floor of the pitch sweep
    double kickPitchExc    = 100.0;   // 150 Hz start - 50 Hz floor
    double kickLife        = 0.300;   // seconds of amplitude tail
    double kickPitchTime   = 0.080;   // seconds to complete the drop

    double snareNoiseLife  = 0.180;
    double snareToneLife   = 0.120;
    double snareToneFreq   = 180.0;
    double snareBPFreq     = 1100.0;  // band-pass center for the noise body
    double snareBPQ        = 1.0;

    double hiHatLife       = 0.045;   // very fast amplitude decay
    double hiHatHPFreq     = 7500.0;  // high-pass corner for "metallic" top
};
constexpr DrumSynthTuning kSynth {};
}

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

    initialiseDrumSynth (sampleRate);

    if (auto* swing = apvts.getParameter ("swing"))
        setSwing (swing->getValue ());
}

void PluginAudioProcessor::initialiseDrumSynth (double sampleRate)
{
    if (sampleRate <= 0.0)
        sampleRate = 48000.0;

    // Exponential-decay multipliers: each model's amplitude envelope reaches
    // tailThreshold over its configured life span.
    kickPitchStep       = std::pow (0.0015, 1.0 / (kSynth.kickPitchTime * sampleRate));
    kickAmpStep         = std::pow (kSynth.tailThreshold, 1.0 / (kSynth.kickLife * sampleRate));
    snareNoiseAmpStep   = std::pow (kSynth.tailThreshold, 1.0 / (kSynth.snareNoiseLife * sampleRate));
    snareToneAmpStep    = std::pow (kSynth.tailThreshold, 1.0 / (kSynth.snareToneLife * sampleRate));
    snareTonePhaseInc   = kTwoPi * kSynth.snareToneFreq / sampleRate;

    // RBJ cookbook band-pass biquad (a0 normalized), Direct Form 1.
    const double w     = kTwoPi * kSynth.snareBPFreq / sampleRate;
    const double alpha = std::sin (w) / (2.0 * kSynth.snareBPQ);
    const double a0    = 1.0 + alpha;
    const double a1    = -2.0 * std::cos (w);
    const double a2    = 1.0 - alpha;
    snareBP[0] = alpha / a0;
    snareBP[1] = 0.0;
    snareBP[2] = -alpha / a0;
    snareBP[3] = a1 / a0;
    snareBP[4] = a2 / a0;

    hiHatAmpStep = std::pow (kSynth.tailThreshold, 1.0 / (kSynth.hiHatLife * sampleRate));
    hiHatHPGain  = 1.0 - std::exp (-kTwoPi * kSynth.hiHatHPFreq / sampleRate);

    // Reset the fixed voice pool and the virtual playhead counter.
    internalPpqPosition = 0.0;
    drumVoiceRoll       = 0;
    voiceSeedCounter    = 1;

    for (auto& voice : drumVoices)
        voice = DrumVoice {};
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

    // The internal drum synth is a Standalone-verify asset: the buffer stays
    // cleared and the generated MIDI passes straight out to the user's drum
    // machine in a DAW (VST3/AU). In Standalone there is no external machine,
    // so the same note-on stream is tapped to render internal audio instead.
    const bool standalone = (wrapperType == juce::AudioProcessor::wrapperType_Standalone);

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

    // Standalone has no transport timeline, so the sequencer must run forever
    // on its own sample-driven virtual playhead at fallbackBpmCache tempo.
    if (standalone && !isPlaying)
    {
        const double ppqPerSample = bpm / (60.0 * sampleRate * 4.0);
        internalPpqPosition += numSamples * ppqPerSample;
        ppqStart = internalPpqPosition;
        isPlaying = true;
    }

    if (isPlaying)
    {
        processPendingNoteOffs (midiMessages, numSamples);
        scheduleLanes (midiMessages, numSamples, ppqStart, bpm, sampleRate);
    }
    else
    {
        stopAndFlush (midiMessages);
    }

    if (standalone)
        renderInternalSynth (buffer, midiMessages, numSamples);

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

// ---------------------------------------------------------------------------
// Internal pre-allocated drum synthesizer (Standalone only).
//
// Task A: kick (sine with fast exponential pitch sweep 150 -> 50 Hz over
// ~80 ms), snare (band-passed white noise + a short 180 Hz sine tone), hihat
// (high-passed white noise with a fast ~45 ms exponential decay). Voices are
// pre-allocated in prepareToPlay() and the processBlock path never touches the
// heap or the disk.
// ---------------------------------------------------------------------------

PluginAudioProcessor::DrumModel PluginAudioProcessor::drumModelForNote (int note)
{
    // Default NOTEMAP pattern (36..51, repeated across the 4 note banks): lane
    // 0 is the kick, lane 1 the snare, lanes 2 and 3 the hi-hat.
    switch (positiveMod (note - 36, kNumLanes))
    {
        case 1:  return DrumModel::snare;
        case 2:
        case 3:  return DrumModel::hihat;
        default: return DrumModel::kick;
    }
}

std::uint32_t PluginAudioProcessor::nextXorshift (std::uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void PluginAudioProcessor::triggerDrumVoice (int note, float velocity01)
{
    const DrumModel model = drumModelForNote (note);

    // Round-robin steal: the voice under drumVoiceRoll is always reclaimed,
    // whether its envelope already died or it is being cut off.
    DrumVoice& voice = drumVoices[(size_t) drumVoiceRoll];
    drumVoiceRoll    = (drumVoiceRoll + 1) % kDrumVoiceCount;

    voice = DrumVoice {};
    voice.model       = model;
    voice.active      = true;
    voice.env         = juce::jlimit (0.0, 1.0, (double) velocity01);

    voice.noiseState  = (voiceSeedCounter += 0x6D2B79F5u) | 1u;
    ++voiceSeedCounter;

    switch (model)
    {
        case DrumModel::kick:
            voice.samplesLeft    = (int) (kSynth.kickLife * currentSampleRate);
            voice.kickPhase      = 0.0;
            break;

        case DrumModel::snare:
            voice.samplesLeft    = (int) (kSynth.snareNoiseLife * currentSampleRate);
            voice.bandX1         = 0.0;
            voice.bandX2         = 0.0;
            voice.bandY1         = 0.0;
            voice.bandY2         = 0.0;
            voice.tonePhase      = 0.0;
            voice.toneAmp        = voice.env;
            break;

        case DrumModel::hihat:
            voice.samplesLeft = (int) (kSynth.hiHatLife * currentSampleRate);
            voice.hpLastX     = 0.0;
            voice.hpLastY     = 0.0;
            break;
    }
}

void PluginAudioProcessor::renderInternalSynth (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midiMessages,
                                                int numSamples)
{
    if (numSamples <= 0)
        return;

    for (const auto& metadata : midiMessages)
    {
        if (metadata.numBytes < 3)
            continue;

        const auto* data = metadata.data;

        if ((data[0] & 0xF0) != 0x90)                  // note-on family only
            continue;

        if ((data[0] & 0x0F) != (kBasicChannel - 1))   // MIDI channel 1 only
            continue;

        const int velocity = data[2];

        if (velocity <= 0)                             // note-offs are ignored
            continue;                                  // (voices decay naturally)

        triggerDrumVoice (data[1], velocity / 127.0f);
    }

    for (auto& voice : drumVoices)
        if (voice.active)
            renderDrumVoice (buffer, voice, numSamples);
}

void PluginAudioProcessor::renderDrumVoice (juce::AudioBuffer<float>& buffer,
                                            DrumVoice& voice,
                                            int numSamples)
{
    const int numChannels = juce::jmax (1, buffer.getNumChannels ());
    const float* const* channels = buffer.getArrayOfWritePointers ();

    for (int s = 0; s < numSamples && s < voice.samplesLeft; ++s)
    {
        double output = 0.0;

        switch (voice.model)
        {
            case DrumModel::kick:
            {
                // 150 Hz wide-open -> closes on 50 Hz; env polynomial adds the
                // familiar punch so the sweep is audible over the tail.
                const double freq = kSynth.kickBaseFreq + kSynth.kickPitchExc * voice.kickPitchExc;
                voice.kickPhase += kTwoPi * freq / currentSampleRate;
                const double phase = std::fmod (voice.kickPhase, kTwoPi);

                const double v = (1.0 - voice.env) * (1.0 - voice.env);
                output = (std::sin (phase) * (1.0 - v) + (phase < kTwoPi * 0.15 ? 1.0 : 0.0) * v)
                         * voice.env;

                voice.kickPitchExc *= kickPitchStep;
                voice.env         *= kickAmpStep;
                break;
            }

            case DrumModel::snare:
            {
                const double noise = nextXorshift (voice.noiseState) * (1.0 / 4294967295.0) * 2.0 - 1.0;

                // Direct Form 1 band-pass biquad over the white noise.
                voice.bandY1 = snareBP[0] * noise
                             + snareBP[1] * voice.bandX1 + snareBP[2] * voice.bandX2
                             - snareBP[3] * voice.bandY1 - snareBP[4] * voice.bandY2;
                voice.bandX2 = voice.bandX1;
                voice.bandX1 = noise;

                const double bandPassed = voice.bandY1;
                const double tone       = std::sin (voice.tonePhase) * voice.toneAmp;

                voice.bandY2 = voice.bandY1;
                voice.tonePhase += snareTonePhaseInc;

                output = bandPassed * (1.0 - voice.env * 0.25) + tone * (voice.env * voice.env);

                voice.env             *= snareNoiseAmpStep;
                voice.toneAmp         *= snareToneAmpStep;
                break;
            }

            case DrumModel::hihat:
            {
                const double noise = nextXorshift (voice.noiseState) * (1.0 / 4294967295.0) * 2.0 - 1.0;

                // First-order high-pass: y = g * (x - x1 + y1).
                const double hp = hiHatHPGain * ((noise - voice.hpLastX) + voice.hpLastY);

                output = hp * voice.env;

                voice.hpLastX = noise;
                voice.hpLastY = hp;
                voice.env    *= hiHatAmpStep;
                break;
            }
        }

        output = juce::jlimit (-1.0, 1.0, output);

        for (int c = 0; c < numChannels; ++c)
            channels[c][s] = (float) output;

        --voice.samplesLeft;
    }

    if (voice.samplesLeft <= 0 || voice.env < kSynth.tailThreshold)
        voice.active = false;
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