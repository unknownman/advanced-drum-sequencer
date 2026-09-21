"""MPD32Sequencer - Shared protocol tables for the Akai MPD32 hardware profile.

This module is a pure-Python, dependency-free contract between the hardware,
the lock-free C++ engine (PluginProcessor) and the Ableton Live remote script
(``__init__.py`` in the same folder).

It is intentionally importable OUTSIDE of Ableton Live so the tables can be
unit-tested and cross-referenced against the C++ constants in
``Source/PluginProcessor.h``.

All MIDI channels are expressed as 0-based indices used by the Live MIDI
Remote Scripts framework (channel 0 == MIDI channel 1).
"""

# ---------------------------------------------------------------------------
# Core sequence topology (must match PluginProcessor.h)
# ---------------------------------------------------------------------------

LANE_COUNT = 16          # kNumLanes
MAX_STEPS_PER_LANE = 64  # kMaxStepsPerLane
AUTOMATED_STEP_COUNT = 32  # kAutomationStepCount (steps exposed as DAW params)
PARAM_COUNT = 1 + LANE_COUNT * AUTOMATED_STEP_COUNT  # swing + 512 step velocities

# ---------------------------------------------------------------------------
# Akai MPD32 hardware MIDI layout (factory defaults, MIDI channel 1)
# ---------------------------------------------------------------------------

CONTROL_CHANNEL = 0   # 0-based -> MIDI channel 1
NOTE_CHANNEL = 0      # 0-based -> MIDI channel 1

# Pad 1..16 -> MIDI notes 36..51 (bank A factory mapping).
PAD_NOTE_BASE = 36
PAD_NOTE_COUNT = 16

# Hardware bank offsets applied by the MPD32's own A/B/C/D bank buttons
# (each bank shifts the pad notes up an octave).
HARDWARE_PAD_BANK_OFFSETS = (0, 12, 24, 36)

# The engine's four software note banks (kNumNoteBanks). Values are matched
# to the hardware bank offsets so pedals, pads and the plugin see one system.
# Each bank shifts the lane notes by 16 to align with Ableton 16-pad Drum Racks.
ENGINE_NOTE_BANK_BASE = (36, 52, 68, 84)  # 36 + lane + bank * 16

# F1..F8 faders -> CC 12..19
FADER_CC_BASE = 12
FADER_COUNT = 8

# K1..K8 knobs -> CC 22..29
KNOB_CC_BASE = 22
KNOB_COUNT = 8

# B1..B8 control buttons -> CC 32..39
BUTTON_CC_BASE = 32
BUTTON_COUNT = 8

# 16 drum pad display names, in engine lane order (matches the editor sidebar)
LANE_NAMES = (
    'Kick', 'Snare', 'Hat Closed', 'Hat Open', 'Clap', 'Tom Low', 'Tom Mid',
    'Tom High', 'Rim', 'Cowbell', 'Shaker', 'Claves', 'Maracas', 'Crash',
    'Ride', 'Perc',
)

# ---------------------------------------------------------------------------
# VST parameter cross-reference (must match PluginProcessor.cpp layout)
# ---------------------------------------------------------------------------

# The AudioProcessorValueTreeState registers parameters in this exact order:
#   index 0                       -> "swing" (normalised 0..1)
#   index 1 + lane*32 + step      -> "lane_%d_step_%d_vel"
#
# The Live script pages the 16 hardware controls (8 faders + 8 knobs) and the
# 16 pads (feedback LEDs) over these parameters with the helper below.
# Page 0 additionally exposes "swing" on slot 0 (F1) for one-shot device setup.

SWING_PARAM_INDEX = 0


def step_velocity_param_index(lane, step):
    """Return the VST parameter index for a lane+step automation parameter."""
    return 1 + lane * AUTOMATED_STEP_COUNT + step


def lane_step_from_param_index(param_index):
    """Reverse lookup of step_velocity_param_index; 0-based lane/step tuple.

    Returns ``None`` for the swing parameter or out-of-range indices.
    """
    if param_index < 1 or param_index >= PARAM_COUNT:
        return None
    offset = param_index - 1
    return (offset // AUTOMATED_STEP_COUNT, offset % AUTOMATED_STEP_COUNT)


def param_name(param_index):
    """Canonical VST parameter name for a parameter index.

    Mirrors the C++ AudioParameterFloat names so the remote script can resolve
    parameters by String Name (Ableton exposes only a 128-parameter window on
    the device object model) instead of relying purely on position.
    """
    if param_index == SWING_PARAM_INDEX:
        return 'Swing'
    spec = lane_step_from_param_index(param_index)
    if spec is None:
        return None
    lane, step = spec
    return 'Lane %d Step %d Velocity' % (lane + 1, step + 1)


_STEPS_PER_PAGE = 16


def control_param_index(page, slot):
    """Map a hardware slot (0..15) on a page (0..31) to a VST parameter index.

    - page 0, slot 0 -> swing parameter (index 0, F1 / pad 1).
    - page 0, slot s (1..15) -> step params 1..15.
    - page p (>= 1), slot s -> step param index ``p * 16 + s``.
    """
    page = max(0, min(31, int(page)))
    slot = max(0, min(15, int(slot)))
    if page == 0:
        return 0 if slot == 0 else slot
    return page * _STEPS_PER_PAGE + slot


def controls_for_page(page):
    """Return ``[(control_name, cc, param_index), ...]`` for one page.

    Faders occupy slots 0..7 (CC 12..19), knobs slots 8..15 (CC 22..29).
    """
    controls = []
    for slot in range(_STEPS_PER_PAGE):
        if slot < FADER_COUNT:
            name = 'F%d' % (slot + 1)
            midi_type = 'cc'
            identifier = FADER_CC_BASE + slot
        else:
            name = 'K%d' % (slot - FADER_COUNT + 1)
            midi_type = 'cc'
            identifier = KNOB_CC_BASE + (slot - FADER_COUNT)
        controls.append((name, midi_type, identifier, control_param_index(page, slot)))
    return controls


# Documents the CC actions implemented by the engine's real-time
# processHardwareController() (active only when the MPD32's control data is
# routed directly to the plugin, i.e. NOT intercepted by the Live script).
MPD32_CC_TABLE = {
    'F1..F8': (FADER_CC_BASE, FADER_CC_BASE + FADER_COUNT - 1,
               'Engine: per-lane velocity trim, then Live-script: VST parameter faders'),
    'K1..K8': (KNOB_CC_BASE, KNOB_CC_BASE + KNOB_COUNT - 1,
               'Engine: per-lane loop length (1..127 -> clamped 1..64), '
               'then Live-script: VST parameter knobs'),
    'B1..B8': (BUTTON_CC_BASE, BUTTON_CC_BASE + BUTTON_COUNT - 1,
               'Engine: toggle MIDI-learn per lane; Live-script: page navigation'),
}