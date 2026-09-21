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

    layout.add (std::make_unique<juce::AudioParameterFloat> ("swing", "Swing",
                                                             juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
                                                             0.5f));

    for (int lane = 0; lane < kNumLanes; ++lane)
        for (int step = 0; step < kAutomationStepCount; ++step)
            layout.add (std::make_unique<juce::AudioParameterFloat> (
                laneStepVelParameterID (lane, step),
                "Lane " + juce::String (lane + 1) + " Step " + juce::String (step + 1) + " Velocity",
                juce::NormalisableRange<float> (0.0f, 127.0f, 0.01f),
                0.0f));

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
}

void PluginAudioProcessor::resetPatternData ()
{
    laneInLearnMode.store (-1, std::memory_order_relaxed);
    swingParamCache.store (0.5f, std::memory_order_relaxed);
    fallbackBpmCache.store (120.0, std::memory_order_relaxed);

    activeNoteBank.store (0, std::memory_order_relaxed);
    mpdLaneBank.store (0, std::memory_order_relaxed);

    for (int bank = 0; bank < kNumNoteBanks; ++bank)
        for (int lane = 0; lane < kNumLanes; ++lane)
            noteBankCaches[(size_t) bank][(size_t) lane].store (36 + lane + bank * 12, std::memory_order_relaxed);

    for (int lane = 0; lane < kNumLanes; ++lane)
    {
        loopLengthCaches[(size_t) lane].store (16, std::memory_order_relaxed);
        targetNoteCaches[(size_t) lane].store (36 + lane, std::memory_order_relaxed);
        velocityScaleCaches[(size_t) lane].store (1.0f, std::memory_order_relaxed);
        currentStepCaches[(size_t) lane].store (-1, std::memory_order_relaxed);

        for (int step = 0; step < kMaxStepsPerLane; ++step)
            stepVelocityCaches[(size_t) lane][(size_t) step].store (0.0f, std::memory_order_relaxed);
    }
}

void PluginAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;

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
    double ppqStart  = 0.0;
    double bpm       = fallbackBpmCache.load (std::memory_order_relaxed);
    double sampleRate = currentSampleRate;

    if (auto* playhead = getPlayHead ())
    {
        if (const auto position = playhead->getPosition ())
        {
            isPlaying = position->getIsPlaying ();

            if (const auto s = position->getSampleRate ())
                sampleRate = *s;

            if (const auto b = position->getBpm ())
                bpm = *b;

            if (const auto ppq = position->getPpqPosition ())
                ppqStart = *ppq;
        }
    }

    currentSampleRate = sampleRate;
    fallbackBpmCache.store (bpm, std::memory_order_relaxed);

    if (isPlaying)
        scheduleLanes (midiMessages, numSamples, ppqStart, bpm, sampleRate);
    else
        stopAndFlush (midiMessages);

    wasPlayingLast = isPlaying;
}

void PluginAudioProcessor::maybeProcessMIDILearn (juce::MidiBuffer& midiMessages)
{
    const int laneToLearn = laneInLearnMode.load (std::memory_order_relaxed);

    if (laneToLearn < 0)
        return;

    for (const auto& metadata : midiMessages)
    {
        const auto& message = metadata.getMessage ();

        if (! message.isNoteOn () || message.getVelocity () == 0)
            continue;

        targetNoteCaches[(size_t) laneToLearn].store (message.getNoteNumber (), std::memory_order_relaxed);
        noteBankCaches[(size_t) activeNoteBank.load (std::memory_order_relaxed)][(size_t) laneToLearn].store (
            message.getNoteNumber (), std::memory_order_relaxed);
        laneInLearnMode.store (-1, std::memory_order_relaxed);
        midiMessages.clear ();
        return;
    }
}

void PluginAudioProcessor::processHardwareController (juce::MidiBuffer& midiMessages)
{
    const int laneBank = mpdLaneBank.load (std::memory_order_relaxed) != 0 ? 8 : 0;

    for (const auto& metadata : midiMessages)
    {
        const auto& message = metadata.getMessage ();

        if (! message.isController ())
            continue;

        const int cc    = message.getControllerNumber ();
        const int value = message.getControllerValue ();

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

void PluginAudioProcessor::scheduleLanes (juce::MidiBuffer& midiMessages, int numSamples,
                                          double ppqStart, double bpm, double sampleRate)
{
    if (numSamples <= 0 || bpm <= 0.0 || sampleRate <= 0.0)
        return;

    const double samplesPerStep = (60.0 * sampleRate * 0.25) / bpm;
    const double blockSpan      = (double) numSamples / samplesPerStep;
    const double fracSixteenth  = ppqStart / 0.25;

    if (! timelinePrimed)
    {
        timelinePrimed = true;

        for (int lane = 0; lane < kNumLanes; ++lane)
            lastSteps[lane] = -1;

        lastSixteenthFired = (int) std::floor (fracSixteenth);
    }

    const bool onExactBoundary = (fracSixteenth == std::floor (fracSixteenth));
    const int startBoundary    = onExactBoundary ? (int) std::floor (fracSixteenth)
                                                 : (int) std::ceil (fracSixteenth);
    const int endBoundaryExclusive = (int) std::ceil (fracSixteenth + blockSpan);

    if (startBoundary < lastSixteenthFired
        || startBoundary - lastSixteenthFired > kMaxStepsPerLane * 2)
    {
        for (int lane = 0; lane < kNumLanes; ++lane)
            lastSteps[lane] = -1;

        lastSixteenthFired = startBoundary - 1;
    }

    for (int sixteenth = startBoundary; sixteenth < endBoundaryExclusive; ++sixteenth)
    {
        const int baseSample = (int) std::floor ((sixteenth - fracSixteenth) * samplesPerStep + 0.5);

        for (int lane = 0; lane < kNumLanes; ++lane)
            renderLaneHit (midiMessages, lane, sixteenth, baseSample, samplesPerStep, numSamples);

        lastSixteenthFired = sixteenth;
    }
}

void PluginAudioProcessor::renderLaneHit (juce::MidiBuffer& midiMessages, int lane,
                                          int globalSixteenth, int baseSample,
                                          double samplesPerStep, int numSamples)
{
    const int loopLength = std::max (1, loopLengthCaches[(size_t) lane].load (std::memory_order_relaxed));
    const int laneStep   = positiveMod (globalSixteenth, loopLength);

    if (laneStep == lastSteps[lane])
        return;

    lastSteps[lane] = laneStep;

    currentStepCaches[(size_t) lane].store (laneStep, std::memory_order_relaxed);

    const float velocity01 = stepVelocityCaches[(size_t) lane][(size_t) laneStep].load (std::memory_order_relaxed);

    if (velocity01 <= 0.0f)
        return;

    const int midiNote = targetNoteCaches[(size_t) lane].load (std::memory_order_relaxed);

    if (midiNote < 0 || midiNote > 127)
        return;

    const float swing = juce::jlimit (0.0f, 1.0f, swingParamCache.load (std::memory_order_relaxed));

    const int swingSamples = (laneStep % 2) == 0
                                 ? 0
                                 : (int) std::lround (samplesPerStep * (double) swing * 0.5);

    const int samplePos     = juce::jlimit (0, numSamples, baseSample + swingSamples);
    const float scale       = juce::jlimit (0.0f, 1.0f,
                                            velocityScaleCaches[(size_t) lane].load (std::memory_order_relaxed));
    const int velocityMidi  = juce::jlimit (1, 127, (int) std::lround (velocity01 * scale * 127.0f));

    midiMessages.addEvent (juce::MidiMessage::noteOn (kBasicChannel, midiNote, (juce::uint8) velocityMidi),
                           samplePos);

    const int noteOffSamples = (int) std::lround (currentSampleRate * 0.08);

    midiMessages.addEvent (juce::MidiMessage::noteOff (kBasicChannel, midiNote, juce::uint8 (0)),
                           juce::jlimit (0, numSamples, samplePos + noteOffSamples));
}

void PluginAudioProcessor::stopAndFlush (juce::MidiBuffer& midiMessages)
{
    if (wasPlayingLast)
    {
        midiMessages.addEvent (juce::MidiMessage::allNotesOff (kBasicChannel), 0);
        wasPlayingLast = false;

        for (int lane = 0; lane < kNumLanes; ++lane)
            currentStepCaches[(size_t) lane].store (-1, std::memory_order_relaxed);
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
    if (lane < 0 || lane >= kNumLanes)
        return;

    targetNoteCaches[(size_t) lane].store (juce::jlimit (0, 127, midiNote), std::memory_order_relaxed);
}

int PluginAudioProcessor::getTargetNote (int lane) const
{
    if (lane < 0 || lane >= kNumLanes)
        return 0;

    return targetNoteCaches[(size_t) lane].load (std::memory_order_relaxed);
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
    const int clampedBank = juce::jlimit (0, kNumNoteBanks - 1, bank);

    activeNoteBank.store (clampedBank, std::memory_order_relaxed);

    for (int lane = 0; lane < kNumLanes; ++lane)
        targetNoteCaches[(size_t) lane].store (
            noteBankCaches[(size_t) clampedBank][(size_t) lane].load (std::memory_order_relaxed),
            std::memory_order_relaxed);
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

    if (clampedBank == activeNoteBank.load (std::memory_order_relaxed))
        targetNoteCaches[(size_t) lane].store (note, std::memory_order_relaxed);
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