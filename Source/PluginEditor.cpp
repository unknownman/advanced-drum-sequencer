#include "PluginEditor.h"

#include <algorithm>

namespace drumseq
{

namespace
{
const char* const kTrackNames[PluginAudioProcessor::kNumLanes] = {
    "Kick",       "Snare",      "Hat Closed", "Hat Open", "Clap",     "Tom Low",
    "Tom Mid",    "Tom High",   "Rim",        "Cowbell",  "Shaker",   "Claves",
    "Maracas",    "Crash",      "Ride",       "Perc"
};

const char* const kSynthParamNames[PluginAudioEditor::kSynthParamCount] = {
    "PITCH", "ATK", "DECAY", "SUSTAIN", "RELEASE", "LFO RT", "LFO DP", "NOISE"
};

const char* const kSynthParamKeys[PluginAudioEditor::kSynthParamCount] = {
    "pitch", "attack", "decay", "sustain", "release", "lfo_rate", "lfo_depth", "noise_blend"
};

// On-hit velocity used by the Euclidean generator, matching the manual pad
// baseline (100 on a 0..127 velocity scale).
constexpr float kEuclidHitVelocity01 = 100.0f / 127.0f;

// Commits a Björklund pattern into all 32 step-velocity states of a lane:
// the sticky cache first (audio thread reads immediately), then each velocity
// parameter via setValueNotifyingHost inside a single host edit gesture so the
// whole 32-step rewrite records as one automation transaction.
void applyEuclideanRhythm (PluginAudioProcessor& processor, int lane, int pulses, int steps)
{
    const auto pattern = PluginAudioProcessor::computeEuclideanRhythm (pulses, steps);

    auto* gestureParam = processor.getLaneStepVelParameter (lane, 0);

    if (gestureParam != nullptr)
        gestureParam->beginChangeGesture ();

    for (int step = 0; step < PluginAudioProcessor::kAutomationStepCount; ++step)
    {
        const float target = pattern[(size_t) step] ? kEuclidHitVelocity01 : 0.0f;

        processor.setStepVelocity (lane, step, target);

        if (auto* velParam = processor.getLaneStepVelParameter (lane, step))
            velParam->setValueNotifyingHost (target);
    }

    if (gestureParam != nullptr)
        gestureParam->endChangeGesture ();
}
}

class PluginAudioEditor::MidiLearnButton final : public juce::Button
{
public:
    explicit MidiLearnButton (int laneIndex)
        : juce::Button ("MIDI Learn"),
          lane (laneIndex)
    {
        setClickingTogglesState (true);
        setTooltip ("Assign a hardware pad or note to this lane");
    }

    void paintButton (juce::Graphics& g, bool, bool) override
    {
        const auto& pal = SequencerDesignSystem::palette;
        const auto area = getLocalBounds ().toFloat ();

        if (area.isEmpty ())
            return;

        const bool armed = getToggleState ();
        const bool hover = isMouseOverOrDragging ();

        g.setColour (armed ? pal.midiLearn.brighter (0.06f)
                           : pal.padOff.brighter (hover ? 0.08f : 0.0f));
        g.fillRoundedRectangle (area, 3.0f);

        if (armed)
        {
            g.setColour (pal.midiLearn.withAlpha (0.35f));
            g.drawRoundedRectangle (area.expanded (2.0f), 4.0f, 2.5f);
        }

        g.setFont (SequencerDesignSystem::sequenceNumberFont ());
        g.setColour (pal.text.withAlpha (armed ? 1.0f : 0.55f));
        g.drawText ("LRN", area.toNearestInt (), juce::Justification::centred);
    }

    int getLane () const noexcept { return lane; }

private:
    const int lane;
};

class PluginAudioEditor::TrackHeaderRow final : public juce::Component
{
public:
    class LaneSelectLabel final : public juce::Label
    {
    public:
        LaneSelectLabel (PluginAudioEditor& editor, int laneIndex)
            : owner (editor), lane (laneIndex)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            owner.showLane (lane);
        }

    private:
        PluginAudioEditor& owner;
        const int lane;
    };

    explicit TrackHeaderRow (PluginAudioEditor& editor, int laneIndex)
        : owner (editor),
          lane (laneIndex)
    {
        nameLabel = std::make_unique<LaneSelectLabel> (editor, lane);
        nameLabel->setFont (SequencerDesignSystem::laneLabelFont ());
        nameLabel->setColour (juce::Label::textColourId, SequencerDesignSystem::palette.text);
        nameLabel->setTooltip ("Click to select this lane and edit its Synth parameters");
        addAndMakeVisible (*nameLabel);

        midiLearnButton = std::make_unique<MidiLearnButton> (lane);
        midiLearnButton->onClick = [this]
        {
            if (midiLearnButton->getToggleState ())
                owner.processor.armMIDILearn (lane);
            else
                owner.processor.disarmMIDILearn ();

            owner.updateTrackHeaders ();
        };
        addAndMakeVisible (*midiLearnButton);

        noteLabel.setFont (SequencerDesignSystem::sequenceNumberFont ());
        noteLabel.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.text);
        noteLabel.setTooltip ("MIDI note number assigned to this lane");
        addAndMakeVisible (noteLabel);

        stepsSlider.setSliderStyle (juce::Slider::IncDecButtons);
        stepsSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 34, 16);
        stepsSlider.setRange (1.0, (double) PluginAudioProcessor::kMaxStepsPerLane, 1.0);
        stepsSlider.setValue (16.0);
        stepsSlider.setTooltip ("Loop length in 16th steps for this lane");
        stepsSlider.onValueChange = [this]
        {
            owner.processor.setLoopLength (lane, (int) stepsSlider.getValue ());
        };
        addAndMakeVisible (stepsSlider);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& pal = SequencerDesignSystem::palette;

        if (selected)
        {
            g.setColour (pal.padOn);
            g.fillRect (getLocalBounds ().removeFromLeft (3));
        }

        g.setColour (pal.gridLineColour ().withAlpha (0.15f));
        g.drawHorizontalLine (getHeight () - 1, 0.0f, (float) getWidth ());
    }

    void resized () override
    {
        auto area = getLocalBounds ().reduced (3, 3);

        const auto learn = area.removeFromRight (34).reduced (0, 2);
        const auto steps = area.removeFromRight (66).reduced (0, 4);
        const auto note  = area.removeFromRight (32);

        midiLearnButton->setBounds (learn);
        stepsSlider.setBounds (steps);
        noteLabel.setBounds (note);
        nameLabel->setBounds (area);
    }

    void setSelected (bool shouldBeSelected)
    {
        const bool changed = (shouldBeSelected != selected);
        selected = shouldBeSelected;

        nameLabel->setColour (juce::Label::textColourId,
                             shouldBeSelected ? SequencerDesignSystem::palette.padOn
                                              : SequencerDesignSystem::palette.text);

        if (changed)
            repaint ();
    }

    void setLaneName (const juce::String& name)
    {
        nameLabel->setText (name, juce::dontSendNotification);
    }

    void syncFromProcessor ()
    {
        const bool armed = owner.processor.getLaneInLearnMode () == lane;

        if (midiLearnButton->getToggleState () != armed)
            midiLearnButton->setToggleState (armed, juce::dontSendNotification);

        noteLabel.setText (juce::String (owner.processor.getTargetNote (lane)),
                           juce::dontSendNotification);

        const double length = (double) owner.processor.getLoopLength (lane);

        if (stepsSlider.getValue () != length)
            stepsSlider.setValue (length, juce::dontSendNotification);
    }

    int getLane () const noexcept { return lane; }

private:
    PluginAudioEditor& owner;
    const int lane;

    std::unique_ptr<LaneSelectLabel> nameLabel;
    juce::Label noteLabel;
    std::unique_ptr<MidiLearnButton> midiLearnButton;
    juce::Slider stepsSlider;

    bool selected = false;
};

PluginAudioEditor::PluginAudioEditor (PluginAudioProcessor& p)
    : juce::AudioProcessorEditor (p),
      processor (p)
{
    setLookAndFeel (&lookAndFeel);
    setResizable (true, false);
    setResizeLimits (800, 480, 2600, 1600);
    setSize (980, 540);

    globalTitle.setFont (SequencerDesignSystem::macroFont ());
    globalTitle.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.padOn);
    globalTitle.setText ("drumSeq", juce::dontSendNotification);
    addAndMakeVisible (globalTitle);

    swingLabel.setFont (SequencerDesignSystem::laneLabelFont ());
    swingLabel.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.text);
    swingLabel.setText ("SWING", juce::dontSendNotification);
    addAndMakeVisible (swingLabel);

    swingSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    swingSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 44, 14);
    swingSlider.setRange (0.0, 1.0, 0.001);
    swingSlider.setValue (0.5);
    addAndMakeVisible (swingSlider);

    swingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getAPVTS (), "swing", swingSlider);

    bankLabel.setFont (SequencerDesignSystem::laneLabelFont ());
    bankLabel.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.text);
    bankLabel.setText ("BANK", juce::dontSendNotification);
    addAndMakeVisible (bankLabel);

    synthPanelTitle.setFont (SequencerDesignSystem::laneLabelFont ());
    synthPanelTitle.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.padOn);
    synthPanelTitle.setText ("SYNTH", juce::dontSendNotification);
    addAndMakeVisible (synthPanelTitle);

    const juce::NormalisableRange<float> sliderRanges[kSynthParamCount] = {
        { -24.0f, 24.0f, 0.5f },      // pitch (semitones)
        { 0.001f, 1.0f, 0.0005f },    // attack (s)
        { 0.001f, 3.0f, 0.0005f },    // decay (s)
        { 0.0f, 1.0f, 0.001f },       // sustain (0..1)
        { 0.001f, 3.0f, 0.0005f },    // release (s)
        { 0.05f, 30.0f, 0.0005f },    // lfo rate (Hz)
        { 0.0f, 1.0f, 0.001f },       // lfo depth (0..1)
        { 0.0f, 1.0f, 0.001f }        // noise blend (0..1)
    };

    for (int i = 0; i < kSynthParamCount; ++i)
    {
        synthParamLabels[(size_t) i].setFont (SequencerDesignSystem::sequenceNumberFont ());
        synthParamLabels[(size_t) i].setColour (juce::Label::textColourId,
                                                SequencerDesignSystem::palette.text);
        synthParamLabels[(size_t) i].setText (kSynthParamNames[(size_t) i], juce::dontSendNotification);
        addAndMakeVisible (synthParamLabels[(size_t) i]);

        synthParamSliders[(size_t) i].setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        synthParamSliders[(size_t) i].setTextBoxStyle (juce::Slider::TextBoxBelow, false, 40, 12);
        synthParamSliders[(size_t) i].setRange (sliderRanges[(size_t) i].start, sliderRanges[(size_t) i].end,
                                                sliderRanges[(size_t) i].interval);
        synthParamSliders[(size_t) i].setTooltip (juce::String (kSynthParamNames[(size_t) i])
                                                  + " for the active lane");
        addAndMakeVisible (synthParamSliders[(size_t) i]);
    }

    lfoWaveComboBox.addItemList ({ "SINE", "TRI", "SAW" }, 1);
    lfoWaveComboBox.setTooltip ("LFO waveform archetype for the active lane");
    addAndMakeVisible (lfoWaveComboBox);

    euclidPulsesLabel.setFont (SequencerDesignSystem::sequenceNumberFont ());
    euclidPulsesLabel.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.text);
    euclidPulsesLabel.setText ("EUC P", juce::dontSendNotification);
    addAndMakeVisible (euclidPulsesLabel);

    euclidStepsLabel.setFont (SequencerDesignSystem::sequenceNumberFont ());
    euclidStepsLabel.setColour (juce::Label::textColourId, SequencerDesignSystem::palette.text);
    euclidStepsLabel.setText ("EUC S", juce::dontSendNotification);
    addAndMakeVisible (euclidStepsLabel);

    euclidPulsesSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    euclidPulsesSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 36, 12);
    euclidPulsesSlider.setRange (0.0, (double) PluginAudioProcessor::kAutomationStepCount, 1.0);
    euclidPulsesSlider.setValue (0.0);
    euclidPulsesSlider.setTooltip ("Euclidean pulses for the active lane (0 = off)");
    addAndMakeVisible (euclidPulsesSlider);

    euclidStepsSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    euclidStepsSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 36, 12);
    euclidStepsSlider.setRange (1.0, (double) PluginAudioProcessor::kAutomationStepCount, 1.0);
    euclidStepsSlider.setValue (16.0);
    euclidStepsSlider.setTooltip ("Euclidean step count for the active lane");
    addAndMakeVisible (euclidStepsSlider);

    // Live drag-and-drop MIDI exporter: the button doubles as its own
    // DragAndDropContainer so an OS-level file drag can leave the window.
    midiDragComponent = std::make_unique<MidiDragExportComponent> (processor);
    addAndMakeVisible (*midiDragComponent);

    rebuildSynthPanel (0);

    const char* const bankNames[] = { "A", "B", "C", "D" };

    for (int bank = 0; bank < (int) bankButtons.size (); ++bank)
    {
        auto button = std::make_unique<juce::ToggleButton> (bankNames[bank]);
        button->setClickingTogglesState (true);
        button->setRadioGroupId (7261);
        button->getProperties ().set ("padLabel", juce::String (bankNames[bank]));
        button->getProperties ().set ("padLabelCentre", "1");
        button->setTooltip ("Switch the 16 lane note assignments to bank "
                            + juce::String (bankNames[bank]));

        juce::ToggleButton* buttonPtr = button.get ();
        buttonPtr->onClick = [this, buttonPtr, bank]
        {
            if (buttonPtr->getToggleState ())
                processor.setActiveNoteBank (bank);
        };
        addAndMakeVisible (*button);
        bankButtons[(size_t) bank] = std::move (button);
    }

    bankButtons[0]->setToggleState (true, juce::dontSendNotification);

    for (int lane = 0; lane < PluginAudioProcessor::kNumLanes; ++lane)
    {
        auto row = std::make_unique<TrackHeaderRow> (*this, lane);
        row->setLaneName (kTrackNames[(size_t) lane]);
        addAndMakeVisible (*row);
        trackHeaders[(size_t) lane] = std::move (row);
    }

    for (int lane = 0; lane < PluginAudioProcessor::kNumLanes; ++lane)
        for (int pad = 0; pad < kPadsPerLane; ++pad)
        {
            auto sequencerPad = std::make_unique<DynamicSequencerPad> (processor, lane, pad);
            addAndMakeVisible (*sequencerPad);
            padGrid[(size_t) lane][(size_t) pad] = std::move (sequencerPad);
        }

    // Child pools are fully allocated: resized() may now dereference them.
    // showLane(0) below performs an explicit layout pass against the real size.
    uiInitialized = true;

    showLane (0);

    // Seed the Euclidean macro snapshot from the persisted tree so a state
    // restore never regenerates over manual pad edits, then register the
    // message-thread listener that drives the generator.
    for (int lane = 0; lane < PluginAudioProcessor::kNumLanes; ++lane)
    {
        const juce::String pulsesID = PluginAudioProcessor::laneEuclideanPulsesParameterID (lane);
        const juce::String stepsID  = PluginAudioProcessor::laneEuclideanStepsParameterID (lane);

        if (auto* pulsesParam = processor.getAPVTS ().getParameter (pulsesID))
            lastEuclidPulses[(size_t) lane] = juce::roundToInt (
                pulsesParam->getNormalisableRange ().convertFrom0to1 (pulsesParam->getValue ()));
        else
            lastEuclidPulses[(size_t) lane] = 0;

        if (auto* stepsParam = processor.getAPVTS ().getParameter (stepsID))
            lastEuclidSteps[(size_t) lane] = juce::roundToInt (
                stepsParam->getNormalisableRange ().convertFrom0to1 (stepsParam->getValue ()));
        else
            lastEuclidSteps[(size_t) lane] = 16;

        processor.getAPVTS ().addParameterListener (pulsesID, this);
        processor.getAPVTS ().addParameterListener (stepsID, this);
    }

    startTimerHz (30);
}

PluginAudioEditor::~PluginAudioEditor ()
{
    stopTimer ();

    for (int lane = 0; lane < PluginAudioProcessor::kNumLanes; ++lane)
    {
        processor.getAPVTS ().removeParameterListener (
            PluginAudioProcessor::laneEuclideanPulsesParameterID (lane), this);
        processor.getAPVTS ().removeParameterListener (
            PluginAudioProcessor::laneEuclideanStepsParameterID (lane), this);
    }

    setLookAndFeel (nullptr);
}

void PluginAudioEditor::showLane (int laneIndex)
{
    selectedLane     = juce::jlimit (0, PluginAudioProcessor::kNumLanes - 1, laneIndex);
    lastPlayheadStep = -2;

    for (int lane = 0; lane < PluginAudioProcessor::kNumLanes; ++lane)
        for (int pad = 0; pad < kPadsPerLane; ++pad)
            padGrid[(size_t) lane][(size_t) pad]->setVisible (lane == selectedLane);

    rebuildSynthPanel (selectedLane);
    updateTrackHeaders ();
    resized ();
}

void PluginAudioEditor::rebuildSynthPanel (int laneIndex)
{
    laneIndex = juce::jlimit (0, PluginAudioProcessor::kNumLanes - 1, laneIndex);

    if (laneIndex == synthPanelLane)
        return;

    synthPanelLane = laneIndex;

    synthPanelTitle.setText ("SYNTH L" + juce::String (synthPanelLane + 1),
                             juce::dontSendNotification);

    for (int i = 0; i < kSynthParamCount; ++i)
    {
        synthParamAttachments[(size_t) i].reset ();
        synthParamAttachments[(size_t) i] =
            std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                processor.getAPVTS (),
                PluginAudioProcessor::laneSynthParameterID (synthPanelLane, kSynthParamKeys[(size_t) i]),
                synthParamSliders[(size_t) i]);
    }

    lfoWaveAttachment.reset ();
    lfoWaveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.getAPVTS (),
        PluginAudioProcessor::laneSynthParameterID (synthPanelLane, "lfo_wave"),
        lfoWaveComboBox);

    euclidPulsesAttachment.reset ();
    euclidPulsesAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getAPVTS (),
        PluginAudioProcessor::laneEuclideanPulsesParameterID (synthPanelLane),
        euclidPulsesSlider);

    euclidStepsAttachment.reset ();
    euclidStepsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getAPVTS (),
        PluginAudioProcessor::laneEuclideanStepsParameterID (synthPanelLane),
        euclidStepsSlider);

    repaint ();
}

void PluginAudioEditor::parameterChanged (const juce::String& parameterID, float newValue)
{
    // Only the Euclidean pulse/step macro pair is of interest here; everything
    // else (velocity/synth state) is already owned by attachments + pads.
    int  lane     = -1;
    bool isPulses = false;

    for (int l = 0; l < PluginAudioProcessor::kNumLanes; ++l)
    {
        if (parameterID == PluginAudioProcessor::laneEuclideanPulsesParameterID (l))
        {
            lane     = l;
            isPulses = true;
            break;
        }

        if (parameterID == PluginAudioProcessor::laneEuclideanStepsParameterID (l))
        {
            lane     = l;
            isPulses = false;
            break;
        }
    }

    if (lane < 0)
        return;

    // APVTS delivers the normalized host value (0..1); rescale onto the
    // generator's natural integer range so the macro reads like its slider.
    const int minimum = isPulses ? 0 : 1;

    int natural = minimum;

    if (auto* macroParam = processor.getAPVTS ().getParameter (
            isPulses ? PluginAudioProcessor::laneEuclideanPulsesParameterID (lane)
                     : PluginAudioProcessor::laneEuclideanStepsParameterID (lane)))
        natural = juce::jlimit (minimum, PluginAudioProcessor::kAutomationStepCount,
                                juce::roundToInt (macroParam->getNormalisableRange ()
                                                      .convertFrom0to1 (newValue)));

    // Debounce: state restores, attachment re-syncs and host zero-notifications
    // routinely replay values that never moved. Regenerating the grid on every
    // such notification would stomp deliberate pad edits, so only a genuine
    // macro change gets through.
    auto& lastSeen = isPulses ? lastEuclidPulses[(size_t) lane]
                              : lastEuclidSteps[(size_t) lane];

    if (natural == lastSeen)
        return;

    lastSeen = natural;

    // Pull the other macro's current value in so a single-macro move still
    // observes the freshest pulse/step pair.
    int other = isPulses ? 16 : 0;

    if (auto* paramsParam = processor.getAPVTS ().getParameter (
            isPulses ? PluginAudioProcessor::laneEuclideanStepsParameterID (lane)
                     : PluginAudioProcessor::laneEuclideanPulsesParameterID (lane)))
        other = juce::roundToInt (
            paramsParam->getNormalisableRange ().convertFrom0to1 (paramsParam->getValue ()));

    const int pulses = isPulses ? natural : other;
    const int steps  = isPulses ? other : natural;

    // Generator off (pulses == 0): leave the lane's manual grid untouched.
    if (pulses < 1 || steps < 1)
        return;

    applyEuclideanRhythm (processor, lane, pulses, steps);
}

void PluginAudioEditor::updateTrackHeaders ()
{
    for (int lane = 0; lane < PluginAudioProcessor::kNumLanes; ++lane)
    {
        trackHeaders[(size_t) lane]->setSelected (lane == selectedLane);
        trackHeaders[(size_t) lane]->syncFromProcessor ();
    }
}

void PluginAudioEditor::updatePlayhead ()
{
    const int currentStep = processor.getCurrentStep (selectedLane);

    if (currentStep == lastPlayheadStep)
        return;

    if (lastPlayheadStep >= 0)
        padGrid[(size_t) selectedLane][(size_t) lastPlayheadStep]->refreshFromEngine ();

    if (currentStep >= 0)
        padGrid[(size_t) selectedLane][(size_t) currentStep]->refreshFromEngine ();

    lastPlayheadStep = currentStep;
}

void PluginAudioEditor::refreshVisiblePads ()
{
    for (int pad = 0; pad < kPadsPerLane; ++pad)
        padGrid[(size_t) selectedLane][(size_t) pad]->refreshFromEngine ();
}

void PluginAudioEditor::timerCallback ()
{
    updateTrackHeaders ();
    updatePlayhead ();
    refreshVisiblePads ();
    rebuildSynthPanel (selectedLane);
}

void PluginAudioEditor::paint (juce::Graphics& g)
{
    const auto& pal = SequencerDesignSystem::palette;

    g.fillAll (pal.background);

    if (sidebarDividerX > 0)
    {
        g.setColour (pal.gridLineColour ().withAlpha (0.30f));
        g.drawVerticalLine ((float) sidebarDividerX, 0.0f, (float) getHeight ());
    }

    g.setColour (pal.gridLineColour ().withAlpha (0.35f));
    g.drawHorizontalLine ((float) (getHeight () - 1), 0.0f, (float) getWidth ());
}

void PluginAudioEditor::resized ()
{
    // Standalone launch-crash guard: setResizeLimits()/setSize() inside the
    // constructor dispatch synchronous resized() calls while the
    // bankButtons/trackHeaders/padGrid pools are still empty (all unique_ptr
    // children null). Dereferencing them would fault at (this == 0) - the
    // immediate-launch SIGSEGV previously observed at Component::setBounds +0x40.
    if (! uiInitialized)
        return;

    auto area = getLocalBounds ().reduced (10, 8);

    auto header = area.removeFromTop (56);

    constexpr float headerGap = 16.0f;

    auto leftHeader = header.removeFromLeft (128);
    globalTitle.setBounds (leftHeader.removeFromTop (24));
    swingLabel.setBounds (leftHeader.removeFromTop (12));
    swingSlider.setBounds (leftHeader);

    header.removeFromLeft (juce::roundToInt (headerGap));

    // Far-right corner of the header: the live MIDI drag source. The exporter
    // doubles as its own DragAndDropContainer so the file drag can leave the
    // plugin window entirely (Finder / DAW clip slot).
    midiDragComponent->setBounds (header.removeFromRight (86).reduced (2, 12));

    bankLabel.setBounds (header.removeFromTop (12));

    auto bankRow = header.removeFromTop (28);

    constexpr float bankGap  = 6.0f;
    const float    bankCell  = (bankRow.getWidth () - bankGap * (float) (bankButtons.size () - 1))
                               / (float) bankButtons.size ();

    for (int i = 0; i < (int) bankButtons.size (); ++i)
    {
        bankButtons[(size_t) i]->setBounds (
            bankRow.getX () + juce::roundToInt ((float) i * (bankCell + bankGap)),
            bankRow.getY (),
            juce::roundToInt (bankCell),
            bankRow.getHeight ());
    }

    // Compact parametric synth panel across the bottom (active lane). The eight
    // sliders (Pitch + ADSR + LFO Rate/Depth + Noise Blend) repaint/sync via the
    // 30 Hz timer + SliderAttachments; the LFO wave combo sits next to the title.
    auto synthPanel = area.removeFromBottom (92);

    synthPanelTitle.setBounds (synthPanel.removeFromLeft (58).reduced (0, 34));

    lfoWaveComboBox.setBounds (synthPanel.removeFromLeft (88).reduced (8, 22));

    // Euclidean macro block: pulse/step rotary pair for the active lane.
    auto euclidPulsesCell = synthPanel.removeFromLeft (54);
    euclidPulsesLabel.setBounds (euclidPulsesCell.removeFromTop (16));
    euclidPulsesSlider.setBounds (euclidPulsesCell.reduced (6, 2));

    auto euclidStepsCell = synthPanel.removeFromLeft (54);
    euclidStepsLabel.setBounds (euclidStepsCell.removeFromTop (16));
    euclidStepsSlider.setBounds (euclidStepsCell.reduced (6, 2));

    constexpr float synthGap  = 4.0f;
    const float    synthCell = (synthPanel.getWidth () - synthGap * (float) (kSynthParamCount - 1))
                               / (float) kSynthParamCount;

    for (int i = 0; i < kSynthParamCount; ++i)
    {
        auto cell = synthPanel.removeFromLeft (juce::roundToInt (synthCell));

        synthParamLabels[(size_t) i].setBounds (cell.removeFromTop (16));
        synthParamSliders[(size_t) i].setBounds (cell.reduced (6, 2));

        if (i < kSynthParamCount - 1)
            synthPanel.removeFromLeft (juce::roundToInt (synthGap));
    }

    const auto sidebar  = area.removeFromLeft (juce::roundToInt (area.getWidth () * 0.25f)).reduced (0, 4);
    const auto workspace = area;

    sidebarDividerX = sidebar.getRight ();

    constexpr float rowGap   = 5.0f;
    const float    rowHeight = (sidebar.getHeight () - rowGap * (float) (trackHeaders.size () - 1))
                               / (float) trackHeaders.size ();

    for (int i = 0; i < (int) trackHeaders.size (); ++i)
    {
        trackHeaders[(size_t) i]->setBounds (sidebar.getX (),
                                             sidebar.getY () + juce::roundToInt ((float) i * (rowHeight + rowGap)),
                                             sidebar.getWidth (),
                                             juce::roundToInt (rowHeight));
    }

    constexpr float padGap = 5.0f;

    const float cellW = (workspace.getWidth () - padGap * (float) (kStepsAcross - 1)) / (float) kStepsAcross;
    const float cellH = (workspace.getHeight () - padGap * (float) (kStepsDown - 1)) / (float) kStepsDown;

    for (int row = 0; row < kStepsDown; ++row)
        for (int col = 0; col < kStepsAcross; ++col)
        {
            const int pad = row * kStepsAcross + col;

            padGrid[(size_t) selectedLane][(size_t) pad]->setBounds (
                workspace.getX () + juce::roundToInt ((float) col * (cellW + padGap)),
                workspace.getY () + juce::roundToInt ((float) row * (cellH + padGap)),
                juce::roundToInt (cellW),
                juce::roundToInt (cellH));
        }
}

}