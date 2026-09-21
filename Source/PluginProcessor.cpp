#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace drumseq
{

PluginAudioProcessor::PluginAudioProcessor ()
    : juce::AudioProcessor (juce::AudioProcessor::BusesProperties ()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo (), false)
                                .withOutput ("Output", juce::AudioChannelSet::stereo (),  true)),
      apvts (*this, nullptr, "Parameters", createParameterLayout ())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout PluginAudioProcessor::createParameterLayout ()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> ("swing", "Swing",
                                                             juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
                                                             0.5f));

    return layout;
}

void PluginAudioProcessor::prepareToPlay (double /*sampleRate*/, int /*samplesPerBlock*/)
{
}

void PluginAudioProcessor::releaseResources ()
{
}

void PluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    buffer.clear ();
    midiMessages.clear ();
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