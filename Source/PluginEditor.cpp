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

    for (int step = 0; step < (int) stepPads.size (); ++step)
    {
        auto pad = std::make_unique<DynamicSequencerPad> (processor, 0, step);
        addAndMakeVisible (*pad);
        stepPads[(size_t) step] = std::move (pad);
    }

    const int demoSteps[] = { 0, 4, 8, 10, 12 };

    for (const int step : demoSteps)
        stepPads[(size_t) step]->setVelocity127 (96.0f);

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

    const auto padArea = area.removeFromTop (64);

    constexpr float gap = 4.0f;
    const int count     = (int) stepPads.size ();
    const float cell    = (padArea.getWidth () - gap * (float) (count - 1)) / (float) count;

    for (int i = 0; i < count; ++i)
    {
        const int cellWidth = juce::roundToInt (cell);

        stepPads[(size_t) i]->setBounds (padArea.getX () + juce::roundToInt (i * (cell + gap)),
                                         padArea.getY (),
                                         cellWidth,
                                         padArea.getHeight ());
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