#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <juce_audio_processors/juce_audio_processors.h>

namespace drumseq
{

class PluginAudioProcessor final : public juce::AudioProcessor,
                                   public juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr int kNumLanes        = 16;
    static constexpr int kMaxStepsPerLane = 64;
    static constexpr int kAutomationStepCount = 32;
    static constexpr int kBasicChannel    = 1;
    static constexpr int kNumNoteBanks    = 4;
    static constexpr int kNoteBankSpacing = 16;              // Ableton 16-pad Drum Rack offset
    static constexpr int kNoteOffQueueCapacity = 512;        // fixed lookahead note-off pool

    struct PendingNoteOff
    {
        std::int64_t remainingSamples = 0;
        int note = 0;
        bool active = false;
    };

    // Internal synthesizer voice models (Standalone verify path only).
    enum class DrumModel
    {
        kick,
        snare,
        hihat
    };

    static constexpr int kDrumVoiceCount = 16;

    // High-precision Exponential ADSR: decay and release stages drop along a
    // downward logarithmic contour (e^{-t/tau}) via constant per-sample
    // multipliers, so no exp() is ever evaluated inside the render loop. The
    // attack stage snaps the transient aggressively, then hands off to the
    // exponential decay curve toward the sustain level.
    struct ExponentialEnvelope
    {
        double amp        = 0.0;
        double attackStep = 0.0;   // linear per-sample rise toward 1.0 (snapped onset)
        double decayK     = 1.0;   // e^{-1/(tau_d * fs)} per-sample multiplier
        double releaseK   = 1.0;   // e^{-1/(tau_r * fs)} per-sample multiplier
        double sustain    = 0.0;   // decay-stage target level
        bool   inAttack   = false;
        bool   inRelease  = false;

        void reset () noexcept
        {
            amp = 0.0; attackStep = 0.0;
            decayK = 1.0; releaseK = 1.0; sustain = 0.0;
            inAttack = inRelease = false;
        }

        void prepare (double attackSec, double decaySec, double releaseSec,
                      double sustainLevel, double sampleRate) noexcept
        {
            const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
            const double a  = juce::jmax (attackSec, 1.0e-4);
            const double d  = juce::jmax (decaySec,  1.0e-4);
            const double r  = juce::jmax (releaseSec, 1.0e-4);

            amp       = 0.0;
            sustain   = juce::jlimit (0.0, 1.0, sustainLevel);
            attackStep = 1.0 / (a * sr);
            decayK    = std::exp (-1.0 / (d * sr));
            releaseK  = std::exp (-1.0 / (r * sr));
            inAttack  = true;
            inRelease = false;
        }

        void noteOff () noexcept { inRelease = true; }

        double getNextSample () noexcept
        {
            if (inAttack)
            {
                amp += attackStep;
                if (amp >= 1.0)
                {
                    amp = 1.0;
                    inAttack = false;
                }
            }
            else if (inRelease)
            {
                amp *= releaseK;
            }
            else
            {
                amp = sustain + (amp - sustain) * decayK;
            }

            return amp;
        }

        bool isAudible () const noexcept { return ! inAttack && amp <= 1.0e-4; }
    };

    // Per-lane parametric synth parameters, surfaced as pre-cached atomic
    // pointers (apvts.getRawParameterValue) so the audio thread never performs
    // string lookups. Stored values are normalized 0..1; the range is kept in
    // the channel struct for instant convertFrom0to1 on the audio thread.
    struct SynthParamChannel
    {
        std::atomic<float>* pitch      = nullptr;
        std::atomic<float>* attack     = nullptr;
        std::atomic<float>* decay      = nullptr;
        std::atomic<float>* sustain    = nullptr;
        std::atomic<float>* release    = nullptr;
        std::atomic<float>* lfoRate    = nullptr;
        std::atomic<float>* lfoDepth   = nullptr;
        std::atomic<float>* lfoWave    = nullptr;
        std::atomic<float>* noiseBlend = nullptr;

        juce::NormalisableRange<float> pitchRange       { -24.0f, 24.0f, 0.5f };
        juce::NormalisableRange<float> attackRange      { 0.001f, 1.0f,  0.0005f };
        juce::NormalisableRange<float> decayRange       { 0.001f, 3.0f,  0.0005f };
        juce::NormalisableRange<float> sustainRange     { 0.0f,   1.0f,  0.001f };
        juce::NormalisableRange<float> releaseRange     { 0.001f, 3.0f,  0.0005f };
        juce::NormalisableRange<float> lfoRateRange     { 0.05f,  30.0f, 0.0005f };
        juce::NormalisableRange<float> lfoDepthRange    { 0.0f,   1.0f,  0.001f };
        juce::NormalisableRange<float> noiseBlendRange  { 0.0f,   1.0f,  0.001f };
    };

    struct DrumVoice
    {
        DrumModel model = DrumModel::kick;
        bool active = false;
        int samplesLeft = 0;
        int noteOffCountdown = 0;
        bool noteOffSent = false;

        float outputGain = 1.0f;      // captured MIDI velocity 0..1

        // Fully polyphonic exponential envelope, driven lock-free by per-lane
        // atomics. Decay/release follow e^{-t/tau}.
        ExponentialEnvelope env;

        double pitchScale = 1.0;      // 2^(semitones / 12)

        // LFO modulation state (captured at hit onset; advanced per-sample by
        // lfoPhaseInc = lfo_rate / sampleRate, no allocations, no per-sample
        // exp/trig beyond the FastMath sin approximation).
        double lfoPhase     = 0.0;
        double lfoPhaseInc  = 0.0;
        float  lfoDepth     = 0.0f;   // 0..1 depth toward oscillator pitch
        int    lfoWave      = 0;      // 0 sine, 1 triangle, 2 sawtooth

        // Dedicated white-noise texture, darkened by a first-order low-pass
        // whose cutoff is derived from the lane's noise_blend coefficient.
        float  noiseBlend   = 0.0f;
        double noiseLPK     = 0.0;
        double noiseLPState = 0.0;

        // Kick: sine-phase oscillator with exponential pitch sweep.
        double kickPhase    = 0.0;
        double kickPitchExc = 0.0;

        // Snare: band-passed white noise + 180 Hz tone.
        std::uint32_t noiseState = 0x9E3779B1u;
        double bandX1 = 0.0, bandX2 = 0.0;
        double bandY1 = 0.0, bandY2 = 0.0;
        double tonePhase = 0.0;

        // Hi-Hat: high-passed noise state.
        double hpLastX = 0.0;
        double hpLastY = 0.0;
    };

    PluginAudioProcessor ();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources () override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor () override;
    bool hasEditor () const override;

    const juce::String getName () const override;

    int getNumPrograms () override;
    int getCurrentProgram () override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    bool acceptsMidi () const override;
    bool producesMidi () const override;
    double getTailLengthSeconds () const override;

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    bool isBusesLayoutSupported (const juce::AudioProcessor::BusesLayout&) const override;

    void parameterChanged (const juce::String&, float) override;

    juce::AudioProcessorValueTreeState& getAPVTS () noexcept { return apvts; }

    static juce::String laneStepVelParameterID (int lane, int step);
    static juce::String laneStepProbParameterID (int lane, int step);
    static juce::String laneSynthParameterID (int lane, const char* paramName);
    static juce::String laneEuclideanPulsesParameterID (int lane);
    static juce::String laneEuclideanStepsParameterID (int lane);
    juce::RangedAudioParameter* getLaneStepVelParameter (int lane, int step);

    // Björklund's Euclidean algorithm: distributes `pulses` onsets as evenly as
    // possible across `steps` positions (0..31 output; only `steps` entries are
    // meaningful). A non-allocating group-compaction port of the canonical
    // reference implementation; callable from the message thread only.
    static std::array<bool, kAutomationStepCount> computeEuclideanRhythm (int pulses, int steps);

    void setStepVelocity (int lane, int step, float value);
    float getStepVelocity (int lane, int step) const;

    void setLoopLength (int lane, int length);
    int getLoopLength (int lane) const;

    void setTargetNote (int lane, int midiNote);
    int getTargetNote (int lane) const;

    void setSwing (float value);
    float getSwing () const;

    void setVelocityScale (int lane, float value);
    float getVelocityScale (int lane) const;

    void setActiveNoteBank (int bank);
    int getActiveNoteBank () const noexcept;

    void setBankNote (int bank, int lane, int midiNote);
    int getBankNote (int bank, int lane) const;

    int getCurrentStep (int lane) const noexcept;

    void armMIDILearn (int lane);
    void disarmMIDILearn ();
    int getLaneInLearnMode () const noexcept;

    void resetPatternData ();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout ();

    void initialiseCaches ();
    void maybeProcessMIDILearn (juce::MidiBuffer&);
    void processHardwareController (juce::MidiBuffer&);
    void processPendingNoteOffs (juce::MidiBuffer&, int numSamples);
    void queueNoteOff (juce::MidiBuffer&, int numSamples, int note, std::int64_t absoluteSample);
    void scheduleLanes (juce::MidiBuffer&, int numSamples, double ppqStart, double bpm, double sampleRate);
    void stopAndFlush (juce::MidiBuffer&);
    void renderLaneHit (juce::MidiBuffer&, int lane, int absoluteSixteenth, int baseSample,
                        double samplesPerStep, int numSamples);

    // Standalone-only internal synthesizer (pre-allocated; never entered in DAW).
    void initialiseDrumSynth (double sampleRate);
    void renderInternalSynth (juce::AudioBuffer<float>&, int numSamples, int hwChannels);
    void renderDrumVoice (juce::AudioBuffer<float>&, DrumVoice&, int numSamples, int hwChannels);
    void triggerDrumVoice (int lane, float velocity01);
    void noteOffDrumVoices ();
    static DrumModel drumModelForLane (int lane);
    static float defaultNoiseBlendForModel (DrumModel model) noexcept;
    static std::uint32_t nextXorshift (std::uint32_t& state);

    static int positiveMod (int value, int modulo);

    juce::AudioProcessorValueTreeState apvts;

    // Pre-rolled scratch surface, reserved once in prepareToPlay() so dense
    // note queues never trigger real-time heap growth.
    juce::MidiBuffer scratchBuffer;

    std::atomic<int>    laneInLearnMode   { -1 };
    std::atomic<float>  swingParamCache   { 0.5f };
    std::atomic<double> fallbackBpmCache  { 120.0 };

    std::array<std::atomic<int>, kNumLanes> loopLengthCaches;
    std::array<std::atomic<float>, kNumLanes> velocityScaleCaches;
    std::array<std::atomic<int>, kNumLanes> currentStepCaches;
    std::array<std::array<std::atomic<int>, kNumLanes>, kNumNoteBanks> noteBankCaches;
    std::array<std::array<std::atomic<float>, kMaxStepsPerLane>, kNumLanes> stepVelocityCaches;

    // Per-step stochastic trigger-probability atomics (lane_[lane]_step_[step]_prob).
    // Populated in prepareToPlay() exactly like the synth channel pointers; a
    // failed juce::Random roll in renderLaneHit drops that step's hit.
    std::array<std::array<std::atomic<float>*, kAutomationStepCount>, kNumLanes> stepProbCaches;

    // Pre-allocated real-time randomness for the probability gates. The member
    // is only ever touched by the audio thread inside renderLaneHit, so no lock
    // or TLS juggling is needed.
    juce::Random probabilityRandom;

    std::atomic<int> activeNoteBank { 0 };
    std::atomic<int> mpdLaneBank    { 0 };

    // Pre-allocated drum-synthesis voice pool. Round-robin voice-stealing;
    // zero heap growth and zero disk I/O after prepareToPlay().
    std::array<DrumVoice, kDrumVoiceCount> drumVoices;
    std::array<SynthParamChannel, kNumLanes> synthParamChannels;
    int drumVoiceRoll     = 0;
    std::uint32_t voiceSeedCounter = 1;
    bool synthArmed = false;

    // Fixed structural synthesis tuning (computed once in prepareToPlay()).
    double kickPitchStep      = 1.0;   // exponential-pitch sweep multiplier
    double snareTonePhaseInc  = 0.0;
    double snareBP[5]         = {};    // normalized biquad band-pass (b0 b1 b2 a1 a2)
    double hiHatHPGain        = 0.0;   // first-order high-pass feedback gain

    // Continuous virtual playhead for Standalone (no host timeline): this
    // sample-driven counter drives scheduleLanes at fallbackBpmCache tempo.
    double internalPpqPosition = 0.0;

    // Persistent, absolute-sample-timestamped note-off queue (audio-thread only
    // memory; never touched by the UI thread).
    std::array<PendingNoteOff, kNoteOffQueueCapacity> noteOffQueue {};
    int noteOffCount = 0;

    bool timelinePrimed = false;
    bool wasPlayingLast = false;
    int lastSixteenthFired = 0;

    // Remembers the absolute 16th that last fired per lane (monotone, modulo
    // applied at render time) so Loop Length 1 retriggers on every boundary.
    int lastSteps[kNumLanes] = {};

    double currentSampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginAudioProcessor)
};

}