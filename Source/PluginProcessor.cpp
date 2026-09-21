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
// here is fixed at compile time; the envelope (attack/decay/sustain/release)
// and pitch are live lane parameters read from pre-cached atomics.
struct DrumSynthTuning
{
    double kickBaseFreq    = 50.0;    // Hz floor of the pitch sweep
    double kickPitchExc    = 100.0;   // 150 Hz start - 50 Hz floor
    double kickPitchTime   = 0.080;   // seconds to complete the drop

    double snareToneFreq   = 180.0;
    double snareBPFreq     = 1100.0;  // band-pass center for the noise body
    double snareBPQ        = 1.0;

    double hiHatHPFreq     = 7500.0;  // high-pass corner for "metallic" top
    double hiHatNoisePole  = 0.5;     // noise-body balance toward the tone

    // Sanity clamps applied when the live ADSR atomics are ingested, so an
    // in-flight automation write can never produce a degenerate envelope.
    double minAttackS      = 0.0001;
    double minDecayS       = 0.0001;
    double minReleaseS     = 0.0001;
    double maxEnvelopeS    = 3.25;    // tail cap: attack + decay + release + margin
};
constexpr DrumSynthTuning kSynth {};

constexpr double kVoiceTailSafetySeconds = 0.05;
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

    // Parametric internal-synth engine: 5 automatable configuration params per
    // lane (Pitch + ADSR). Being declared directly on the APVTS layout tree,
    // they inherit the XML state serialization layer for free, so presets and
    // Ableton project saves round-trip the full synthesis setup.
    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        const auto pitchRange = juce::NormalisableRange<float> (-24.0f, 24.0f, 0.5f)
                                    .withStringFromValueFunction ([] (float v)
                                                                  {
                                                                      return juce::String::formatted ("%+d st", juce::roundToInt (v));
                                                                  });

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            laneSynthParameterID (lane, "pitch"),
            "Lane " + juce::String (lane + 1) + " Synth Pitch",
            pitchRange,
            0.0f));

        const auto timeRange = [] (float hi)
        {
            return juce::NormalisableRange<float> (0.001f, hi, 0.0005f)
                .withStringFromValueFunction ([] (float v)
                                              {
                                                  if (v >= 0.995f)
                                                      return juce::String::formatted ("%.2f s", v);
                                                  return juce::String::formatted ("%.1f ms", v * 1000.0f);
                                              });
        };

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            laneSynthParameterID (lane, "attack"),
            "Lane " + juce::String (lane + 1) + " Synth Attack",
            timeRange (1.0f),
            0.001f));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            laneSynthParameterID (lane, "decay"),
            "Lane " + juce::String (lane + 1) + " Synth Decay",
            timeRange (3.0f),
            0.25f));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            laneSynthParameterID (lane, "sustain"),
            "Lane " + juce::String (lane + 1) + " Synth Sustain",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f)
                .withStringFromValueFunction ([] (float v)
                                              {
                                                  return juce::String (juce::roundToInt (v * 100.0f)) + "%";
                                              }),
            0.0f));

        layout.add (std::make_unique<juce::AudioParameterFloat> (
            laneSynthParameterID (lane, "release"),
            "Lane " + juce::String (lane + 1) + " Synth Release",
            timeRange (3.0f),
            0.1f));
    }

    return layout;
}

juce::String PluginAudioProcessor::laneSynthParameterID (int lane, const char* paramName)
{
    return juce::String::formatted ("lane_%d_synth_%s", lane, paramName);
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

    // Cache the live ADSR / pitch atomics once. apvts.getRawParameterValue
    // returns stable pointers owned by the tree; the UI/automation thread
    // updates them and the audio thread only ever load()s them.
    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        auto& channel = synthParamChannels[(size_t) lane];

        channel.pitch   = apvts.getRawParameterValue (laneSynthParameterID (lane, "pitch"));
        channel.attack  = apvts.getRawParameterValue (laneSynthParameterID (lane, "attack"));
        channel.decay   = apvts.getRawParameterValue (laneSynthParameterID (lane, "decay"));
        channel.sustain = apvts.getRawParameterValue (laneSynthParameterID (lane, "sustain"));
        channel.release = apvts.getRawParameterValue (laneSynthParameterID (lane, "release"));
    }

    // Exponential pitch-sweep multiplier for the kick model.
    kickPitchStep = std::pow (0.0015, 1.0 / (kSynth.kickPitchTime * sampleRate));

    // Snare: RBJ cookbook band-pass biquad (a0 normalized), Direct Form 1.
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

    snareTonePhaseInc = kTwoPi * kSynth.snareToneFreq / sampleRate;

    // First-order high-pass coefficient for the hi-hat model.
    hiHatHPGain = 1.0 - std::exp (-kTwoPi * kSynth.hiHatHPFreq / sampleRate);

    // Reset the fixed voice pool, the virtual playhead counter, and seed every
    // pre-allocated juce::ADSR with the current sample rate.
    internalPpqPosition = 0.0;
    drumVoiceRoll       = 0;
    voiceSeedCounter    = 1;

    for (auto& voice : drumVoices)
    {
        voice = DrumVoice {};
        voice.adsr.setSampleRate (sampleRate);
    }
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

    synthArmed = standalone;

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
    // Audio-thread hygiene: elapsed time is derived ONLY from the incoming
    // sample block length + sample rate (never std::chrono / wall clock), so
    // the PPQ accumulator is sample-accurate across block boundaries.
    // When a host DAW playhead exists (wrapperType != Standalone) this branch
    // is never entered and the virtual clock sleeps completely: the plugin
    // stays 100% processing-neutral inside Ableton Live.
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

    // Hardware driver routing: the standalone audio device may expose fewer
    // channels than the plugin's stereo architecture (e.g. a mono interface).
    // Clamp the render pass to the channels the driver actually provides and
    // let renderInternalSynth() downmix the summed stereo program + per-sample
    // energy clip into them, so the outbound bus is never written out of bounds
    // and no float accumulation can overflow the driver buffer.
    if (standalone)
    {
        const int hwChannels = juce::jlimit (0, 2, buffer.getNumChannels ());

        if (hwChannels > 0)
            renderInternalSynth (buffer, numSamples, hwChannels);
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

    // Standalone: converge the scheduler hit directly into the pre-allocated
    // parametric voice engine (envelope.noteOn with the lane's cached ADSR).
    if (synthArmed)
        triggerDrumVoice (lane, juce::jlimit (0.0f, 1.0f, velocity01 * scale));

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

        noteOffDrumVoices ();
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
// Internal pre-allocated parametric drum synthesizer (Standalone only).
//
// Lane row topology is fixed by the 16-pad drum map:
//   Lanes 0, 4, 8, 12        (row A) -> low-freq kick: sine + exp pitch sweep
//   Lanes 1, 5, 9, 13        (row B) -> mid-freq snare: band-passed noise + tone
//   Lanes 2, 3, 6, 7, 10,
//   11, 14, 15               (rows C/D) -> metallic hi-hat/percussion
// Each hit reads the lane's live pitch + ADSR atomics and runs a dedicated
// pre-allocated juce::ADSR voice. No heap, no strings, no allocations.
// ---------------------------------------------------------------------------

PluginAudioProcessor::DrumModel PluginAudioProcessor::drumModelForLane (int lane)
{
    switch (positiveMod (lane, 4))
    {
        case 0:  return DrumModel::kick;
        case 1:  return DrumModel::snare;
        default: return DrumModel::hihat;
    }
}

std::uint32_t PluginAudioProcessor::nextXorshift (std::uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void PluginAudioProcessor::triggerDrumVoice (int lane, float velocity01)
{
    if (lane < 0 || lane >= kNumLanes)
        return;

    const auto& channel = synthParamChannels[(size_t) lane];

    // Lock-free reads: these atomics are updated by the APVTS on the UI /
    // automation thread; the audio thread only ever loads them. The pointers
    // are cached in prepareToPlay() but guarded here for full safety.
    const auto loadOr = [] (std::atomic<float>* ptr, float fallback)
    {
        return ptr != nullptr ? ptr->load (std::memory_order_relaxed) : fallback;
    };

    // Fallbacks are normalized values that convert to the layout defaults.
    const float attack   = juce::jmax (channel.attackRange.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, loadOr (channel.attack, 0.0f))),
                                       (float) kSynth.minAttackS);
    const float decay    = juce::jmax (channel.decayRange.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, loadOr (channel.decay, 0.083f))),
                                       (float) kSynth.minDecayS);
    const float sustain  = juce::jlimit (0.0f, 1.0f, loadOr (channel.sustain, 0.0f));
    const float release  = juce::jmax (channel.releaseRange.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, loadOr (channel.release, 0.033f))),
                                       (float) kSynth.minReleaseS);
    const float pitchSemitones = channel.pitchRange.convertFrom0to1 (
        juce::jlimit (0.0f, 1.0f, loadOr (channel.pitch, 0.5f)));

    // Round-robin steal: the voice under drumVoiceRoll is always reclaimed,
    // whether its envelope already died or it is being cut off.
    DrumVoice& voice = drumVoices[(size_t) drumVoiceRoll];
    drumVoiceRoll    = (drumVoiceRoll + 1) % kDrumVoiceCount;

    voice = DrumVoice {};
    voice.model       = drumModelForLane (lane);
    voice.active      = true;
    voice.outputGain  = juce::jlimit (0.0f, 1.0f, velocity01);
    voice.pitchScale  = std::pow (2.0, pitchSemitones / 12.0);

    voice.adsr.setSampleRate (currentSampleRate);
    voice.adsrParams.attack  = attack;
    voice.adsrParams.decay   = decay;
    voice.adsrParams.sustain = sustain;
    voice.adsrParams.release = release;

    // Percussive hits are staccato: after attack + decay the initial transient
    // is spent, so force the release stage (envelope tail) shortly afterwards.
    // The cap guarantees the fixed pool always reclaims the voice.
    voice.noteOffCountdown = (int) ((attack + decay) * currentSampleRate);
    voice.samplesLeft      = (int) ((kSynth.maxEnvelopeS + kVoiceTailSafetySeconds) * currentSampleRate);

    voice.adsr.setParameters (voice.adsrParams);
    voice.adsr.noteOn ();

    voice.noiseState = (voiceSeedCounter += 0x6D2B79F5u) | 1u;

    switch (voice.model)
    {
        case DrumModel::kick:
            voice.kickPhase      = 0.0;
            voice.kickPitchExc   = 1.0;   // full sweep excursion (150 -> 50 Hz)
            break;

        case DrumModel::snare:
            voice.bandX1         = 0.0;
            voice.bandX2         = 0.0;
            voice.bandY1         = 0.0;
            voice.bandY2         = 0.0;
            voice.tonePhase      = 0.0;
            break;

        case DrumModel::hihat:
            voice.hpLastX        = 0.0;
            voice.hpLastY        = 0.0;
            break;
    }
}

void PluginAudioProcessor::noteOffDrumVoices ()
{
    for (auto& voice : drumVoices)
        if (voice.active)
            voice.adsr.noteOff ();
}

void PluginAudioProcessor::renderInternalSynth (juce::AudioBuffer<float>& buffer, int numSamples, int hwChannels)
{
    if (numSamples <= 0 || hwChannels <= 0)
        return;

    for (auto& voice : drumVoices)
        if (voice.active)
            renderDrumVoice (buffer, voice, numSamples, hwChannels);

    // Sum of up to 16 voices must remain within a safe output range. This also
    // performs the mono downmix: when the device exposes a single channel, the
    // summed stereo program is energy-clipped into that one hardware line.
    for (int c = 0; c < hwChannels; ++c)
    {
        auto* channel = buffer.getWritePointer (c);

        for (int s = 0; s < numSamples; ++s)
            channel[s] = juce::jlimit (-1.0f, 1.0f, channel[s]);
    }
}

void PluginAudioProcessor::renderDrumVoice (juce::AudioBuffer<float>& buffer,
                                            DrumVoice& voice,
                                            int numSamples,
                                            int hwChannels)
{
    const int numChannels = juce::jlimit (1, hwChannels, buffer.getNumChannels ());
    const float* const* channels = buffer.getArrayOfWritePointers ();

    const double invSampleRate = 1.0 / currentSampleRate;

    for (int s = 0; s < numSamples && voice.active; ++s)
    {
        if (voice.noteOffCountdown > 0)
            --voice.noteOffCountdown;

        if (voice.noteOffCountdown == 0 && ! voice.noteOffSent)
        {
            voice.noteOffSent = true;
            voice.adsr.noteOff ();
        }

        const double env = voice.adsr.getNextSample ();

        if (env <= 0.0 && ! voice.adsr.isActive ())
        {
            voice.active = false;
            break;
        }

        if (voice.samplesLeft <= 0)
        {
            voice.active = false;
            break;
        }

        --voice.samplesLeft;

        double output = 0.0;

        switch (voice.model)
        {
            case DrumModel::kick:
            {
                // Sine oscillator, exponential pitch sweep 150 -> 50 Hz, scaled
                // live by the lane's Synth Pitch parameter.
                const double freq = (kSynth.kickBaseFreq + kSynth.kickPitchExc * voice.kickPitchExc)
                                    * voice.pitchScale;
                voice.kickPhase += kTwoPi * freq * invSampleRate;
                voice.kickPitchExc *= kickPitchStep;

                output = std::sin (voice.kickPhase) * env;
                break;
            }

            case DrumModel::snare:
            {
                const double noise = nextXorshift (voice.noiseState) * (1.0 / 4294967295.0) * 2.0 - 1.0;

                // Direct Form 1 band-pass biquad over the white noise.
                const double prevY1 = voice.bandY1;
                const double prevY2 = voice.bandY2;

                voice.bandY1 = snareBP[0] * noise
                             + snareBP[1] * voice.bandX1 + snareBP[2] * voice.bandX2
                             - snareBP[3] * prevY1 - snareBP[4] * prevY2;
                voice.bandY2 = prevY1;
                voice.bandX2 = voice.bandX1;
                voice.bandX1 = noise;

                const double bandPassed = voice.bandY1;
                const double tone = std::sin (voice.tonePhase);
                voice.tonePhase += snareTonePhaseInc * voice.pitchScale;

                output = bandPassed * (0.55 + 0.45 * env) + tone * (env * env);
                break;
            }

            case DrumModel::hihat:
            {
                const double noise = nextXorshift (voice.noiseState) * (1.0 / 4294967295.0) * 2.0 - 1.0;

                // First-order high-pass: y = g * (x - x1 + y1).
                const double hp = hiHatHPGain * ((noise - voice.hpLastX) + voice.hpLastY);

                output = hp * env * (1.0 - kSynth.hiHatNoisePole);

                voice.hpLastX = noise;
                voice.hpLastY = hp;
                break;
            }
        }

        output *= voice.outputGain;
        output = juce::jlimit (-1.0, 1.0, output);

        for (int c = 0; c < numChannels; ++c)
            channels[c][s] += (float) output;   // accumulate: 16 voices share the bus
    }

    if (voice.samplesLeft <= 0 || ! voice.adsr.isActive ())
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