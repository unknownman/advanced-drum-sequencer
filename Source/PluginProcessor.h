#pragma once

#include <array>
#include <atomic>
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
    juce::RangedAudioParameter* getLaneStepVelParameter (int lane, int step);

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

    std::atomic<int> activeNoteBank { 0 };
    std::atomic<int> mpdLaneBank    { 0 };

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