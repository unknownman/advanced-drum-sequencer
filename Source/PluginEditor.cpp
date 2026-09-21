#include "PluginEditor.h"

namespace drumseq
{

PluginAudioEditor::PluginAudioEditor (PluginAudioProcessor& p)
    : juce::AudioProcessorEditor (p),
      processor (p)
{
    setLookAndFeel (&lookAndFeel);
    setResizable (true, false);
    setSize (520, 300);

    globalTitle.setFont (SequencerDesignSystem::macroFont ());
    globalTitle.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.padOn);
    globalTitle.setText ("drumSeq", juce::dontSendNotification);
    addAndMakeVisible (globalTitle);

    for (int i = 0; i < (int) pads.size (); ++i)
    {
        auto& pad = pads[(size_t) i];

        pad.getProperties().set ("padLabel", juce::String (i + 1).paddedLeft ('0', 2));
        pad.getProperties().set ("velocity", 0.30f + (i % 3) * 0.28f);

        pad.setToggleState (i % 3 == 0, juce::dontSendNotification);

        addAndMakeVisible (pad);
    }

    pads[6].getProperties().set ("midiLearn", true);
    pads[6].setToggleState (false, juce::dontSendNotification);

    swingLabel.setFont (SequencerDesignSystem::laneLabelFont ());
    swingLabel.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.text);
    swingLabel.setText ("Swing", juce::dontSendNotification);
    addAndMakeVisible (swingLabel);

    swingSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    swingSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 52, 16);
    swingSlider.setRange (0.0, 1.0, 0.001);
    addAndMakeVisible (swingSlider);

    stepLabel.setFont (SequencerDesignSystem::laneLabelFont ());
    stepLabel.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.text);
    stepLabel.setText ("Steps", juce::dontSendNotification);
    addAndMakeVisible (stepLabel);

    stepCounter.setSliderStyle (juce::Slider::IncDecButtons);
    stepCounter.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 42, 20);
    stepCounter.setRange (1.0, 64.0, 1.0);
    stepCounter.setValue (16.0);
    addAndMakeVisible (stepCounter);

    swingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getAPVTS (), "swing", swingSlider);
}

PluginAudioEditor::~PluginAudioEditor ()
{
    setLookAndFeel (nullptr);
}

void PluginAudioEditor::paint (juce::Graphics& g)
{
    const auto& pal = SequencerDesignSystem::palette;

    g.fillAll (pal.background);

    g.setColour (pal.gridLineColour ().withAlpha (0.10f));

    for (int x = 0; x <= getWidth (); x += 12)
        g.drawVerticalLine (x, 0.0f, (float) getHeight ());

    for (int y = 0; y <= getHeight (); y += 12)
        g.drawHorizontalLine (y, 0.0f, (float) getWidth ());

    g.setColour (pal.gridLineColour ().withAlpha (0.35f));
    g.drawHorizontalLine (getHeight () - 1, 0.0f, (float) getWidth ());
}

void PluginAudioEditor::resized ()
{
    auto area = getLocalBounds ().reduced (14, 12);

    globalTitle.setBounds (area.removeFromTop (22));

    const auto padArea = area.removeFromTop (92);

    constexpr float gap = 4.0f;
    constexpr int columns = 4;
    constexpr int rows = 2;

    const float cell = juce::jmin ((padArea.getWidth () - gap * (float) (columns - 1)) / (float) columns,
                                   (padArea.getHeight () - gap * (float) (rows - 1)) / (float) rows);

    for (int i = 0; i < (int) pads.size (); ++i)
    {
        const int column = i % columns;
        const int row    = i / columns;

        pads[(size_t) i].setBounds (padArea.getX () + juce::roundToInt (column * (cell + gap)),
                                    padArea.getY () + juce::roundToInt ((float) row * (cell + gap)),
                                    juce::roundToInt (cell),
                                    juce::roundToInt (cell));
    }

    auto controls = area.removeFromTop (78);

    auto swingGroup = controls.removeFromLeft (controls.getWidth () / 2);
    swingLabel.setBounds (swingGroup.removeFromTop (16));
    swingSlider.setBounds (swingGroup.withSizeKeepingCentre (juce::jmin (54, swingGroup.getWidth ()),
                                                             juce::jmin (54, swingGroup.getHeight ())));

    auto stepGroup = controls.removeFromLeft (controls.getWidth () / 2);
    stepLabel.setBounds (stepGroup.removeFromTop (16));
    stepCounter.setBounds (stepGroup.removeFromTop (24).withTrimmedLeft (2));
}

}