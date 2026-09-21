#pragma once

#include <array>
#include <atomic>

#include <juce_audio_processors/juce_audio_processors.h>

namespace drumseq
{

class PluginAudioProcessor final : public juce::AudioProcessor,
                                   public juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr int kNumLanes      = 16;
    static constexpr int kMaxStepsPerLane = 64;
    static constexpr int kAutomationStepCount = 32;
    static constexpr int kBasicChannel  = 1;

    PluginAudioProcessor ();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources () override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor () override;
    bool hasEditor () const override;

    const juce::String getName () const override;

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

    void armMIDILearn (int lane);
    void disarmMIDILearn ();
    int getLaneInLearnMode () const noexcept;

    void resetPatternData ();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout ();

    void initialiseCaches ();
    void maybeProcessMIDILearn (juce::MidiBuffer&);
    void scheduleLanes (juce::MidiBuffer&, int numSamples, double ppqStart, double bpm, double sampleRate);
    void stopAndFlush (juce::MidiBuffer&);
    void renderLaneHit (juce::MidiBuffer&, int lane, int globalSixteenth, int baseSample,
                        double samplesPerStep, int numSamples);

    static int positiveMod (int value, int modulo);

    juce::AudioProcessorValueTreeState apvts;

    std::atomic<int>    laneInLearnMode   { -1 };
    std::atomic<float>  swingParamCache   { 0.5f };
    std::atomic<double> fallbackBpmCache  { 120.0 };

    std::array<std::atomic<int>, kNumLanes> targetNoteCaches;
    std::array<std::atomic<int>, kNumLanes> loopLengthCaches;
    std::array<std::array<std::atomic<float>, kMaxStepsPerLane>, kNumLanes> stepVelocityCaches;

    bool timelinePrimed = false;
    bool wasPlayingLast = false;
    int lastSixteenthFired = 0;
    int lastSteps[kNumLanes] = {};

    double currentSampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginAudioProcessor)
};

}