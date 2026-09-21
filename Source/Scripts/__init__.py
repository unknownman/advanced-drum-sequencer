"""MPD32Sequencer - Ableton Live MIDI Remote Script for the drumSeq engine.

Drop-in installation
--------------------
Copy this ``__init__.py`` together with ``MPD32SequencerMap.py`` into::

    /Applications/Ableton Live 11 Suite.app/Contents/App-Resources/MIDI Remote Scripts/MPD32Sequencer/

(Adapt the Live product and major version to taste.) Restart Live, open
Preferences -> MIDI -> Control Surface and choose ``MPD32Sequencer`` with the
MPD32 as both input and output ports.

Behaviour
---------
* Recognises the MPD32 over USB by sending a Universal SysEx identity request
  and matching the M-Audio identity reply (``00 00 65``). The profile is
  considered active once the handshake completes or the request times out.
* Binds the 8 faders (CC 12..19) and 8 knobs (CC 22..29) to the focused
  ``drumSeq`` VST3/AU instance's automatable parameters on the selected
  track. The 16 pads (notes 36..51) mirror the same parameters as feedback
  LEDs; pad 1 / fader 1 exposes the plugin's master swing on page 0.
* Any change of DAW track selection re-routes the hardware to the sequencer
  instance on screen (B1/B2 step the 16-control parameter page).
* All callbacks run exclusively on Ableton's UI thread through the framework's
  lock-free listener API (ControlSurface / Component / EncoderElement) - no
  polling loops, no realtime thread work, no blocking calls.
"""

from __future__ import absolute_import

# ---------------------------------------------------------------------------
# Framework imports - version tolerant across Live 10 (legacy _Framework) and
# Live 11/12 (ableton.v2) while remaining valid Python 3.
# ---------------------------------------------------------------------------

try:
    from ableton.v2.control_surface import ControlSurface, Component
    from ableton.v2.control_surface.elements import (EncoderElement,
                                                     SliderElement,
                                                     ButtonElement,
                                                     MapMode)
except ImportError:
    from _Framework.ControlSurface import ControlSurface
    from _Framework.ControlSurfaceComponent import ControlSurfaceComponent as Component
    from _Framework.SliderElement import SliderElement
    from _Framework.ButtonElement import ButtonElement
    try:
        from _Framework.EncoderElement import EncoderElement, MapMode
    except ImportError:
        from _Framework.EncoderElement import EncoderElement
        MapMode = type('MapMode', (), {'Absolute': 0})

from _Framework.InputControlElement import MIDI_CC_TYPE, MIDI_NOTE_TYPE

from MPD32SequencerMap import (FADER_CC_BASE, FADER_COUNT, KNOB_CC_BASE,
                               KNOB_COUNT, BUTTON_CC_BASE, BUTTON_COUNT,
                               PAD_NOTE_BASE, PAD_NOTE_COUNT, CONTROL_CHANNEL,
                               NOTE_CHANNEL, control_param_index)

MAX_PAGE = 31


# ---------------------------------------------------------------------------
# Pure helpers (no framework dependency, unit-testable)
# ---------------------------------------------------------------------------

def identity_matches(message):
    """Match an M-Audio Universal SysEx identity reply.

    ``F0 7E <device> 06 02 00 00 65 ... F7`` where the M-Audio manufacturer
    ID is ``0x00 0x00 0x65``.
    """
    if not isinstance(message, tuple):
        return False
    if len(message) < 9 or not message or message[0] != 0xF0 or message[-1] != 0xF7:
        return False
    return (message[1] == 0x7E and message[3] == 0x06 and message[4] == 0x02
            and message[5] == 0x00 and message[6] == 0x00 and message[7] == 0x65)


def find_drumseq_device(track):
    """Return the first plug-in device on a track, preferring drumSeq."""
    if track is None:
        return None
    devices = getattr(track, 'devices', None) or []
    for device in devices:
        class_name = (getattr(device, 'class_name', None) or '').lower()
        name = (getattr(device, 'name', None) or '').lower()
        if 'drumseq' in class_name or 'drumseq' in name:
            return device
    return devices[0] if devices else None


def _normalise(value):
    """Clamp an incoming element value (0..127) to a live parameter (0..1)."""
    try:
        raw = float(value)
    except (TypeError, ValueError):
        return 0.0
    return max(0.0, min(1.0, raw / 127.0))


def _midi_byte(value):
    """Convert a normalised parameter value to a 7-bit MIDI byte 0..127."""
    try:
        raw = float(value)
    except (TypeError, ValueError):
        return 0
    return int(round(max(0.0, min(1.0, raw)) * 127.0))


# ---------------------------------------------------------------------------
# SequencerDeviceComponent - track-focus aware VST parameter binder
# ---------------------------------------------------------------------------

class SequencerDeviceComponent(Component):
    """Binds hardware controls to the focused drumSeq instance's parameters.

    The component never touches the audio/realtime thread. Every interaction
    flows through the framework's value-listener callbacks which Ableton
    services on its UI thread; rebinding only removes/adds Python listeners.
    """

    def __init__(self, name='SequencerDevice'):
        try:
            super(SequencerDeviceComponent, self).__init__(name=name)
        except TypeError:
            super(SequencerDeviceComponent, self).__init__()
        self._device = None
        self._page = 0
        self._controls = []
        self._pads = []
        self._element_listeners = []  # [(element, callback), ...]
        self._param_listeners = []    # [(param, callback), ...]

    # -- wired by the ControlSurface -------------------------------------
    def set_faders(self, faders):
        self._controls[:FADER_COUNT] = list(faders)
        self._rebind()

    def set_knobs(self, knobs):
        base = FADER_COUNT
        self._controls[base:base + KNOB_COUNT] = list(knobs)
        self._rebind()

    def set_pads(self, pads):
        self._pads = list(pads)
        self._rebind()

    def set_page(self, page):
        page = max(0, min(MAX_PAGE, int(page)))
        if page != self._page:
            self._page = page
            self._rebind(force_feedback=True)

    # -- (de)registration -------------------------------------------------
    def _teardown(self):
        for element, callback in self._element_listeners:
            if element is not None:
                try:
                    element.remove_value_listener(callback)
                except (TypeError, ValueError):
                    pass
        self._element_listeners = []

        for param, callback in self._param_listeners:
            if param is not None:
                try:
                    param.remove_value_listener(callback)
                except (TypeError, ValueError):
                    pass
        self._param_listeners = []

    def _bind_slot(self, element, slot):
        """Register a hardware element against one 16-slot page position."""
        if element is None:
            return

        param_index = control_param_index(self._page, slot)
        params = getattr(self._device, 'parameters', None) or []
        param = params[param_index] if param_index < len(params) else None

        element_cb = lambda value, p=param: self._set_parameter(p, value)
        element.add_value_listener(element_cb)
        self._element_listeners.append((element, element_cb))

        if param is not None:
            param_cb = lambda value, e=element: self._report_parameter(e, value)
            param.add_value_listener(param_cb)
            self._param_listeners.append((param, param_cb))

            self._report_parameter(element, param.value)

    def _rebind(self, force_feedback=False):
        self._teardown()
        if self._device is None:
            return

        for slot, element in enumerate(self._controls[:16]):
            self._bind_slot(element, slot)

        for slot, element in enumerate(self._pads[:16]):
            self._bind_slot(element, slot)

    # -- callbacks (all UI thread) ---------------------------------------
    @staticmethod
    def _set_parameter(param, value):
        if param is None:
            return
        try:
            param.value = _normalise(value)
        except (TypeError, ValueError):
            pass

    @staticmethod
    def _report_parameter(element, value):
        if element is None:
            return
        try:
            element.send_value(_midi_byte(value))
        except (TypeError, ValueError):
            pass


# ---------------------------------------------------------------------------
# ControlSurface
# ---------------------------------------------------------------------------

class MPD32Sequencer(ControlSurface):

    def __init__(self, c_instance):
        super(MPD32Sequencer, self).__init__(c_instance)
        self._identity_seen = False
        self._verified = False
        self._focus_connected = False

        guard = getattr(self, 'component_guard', None)
        if callable(guard):
            with guard():
                self._create_elements()
                self._create_component()
        else:
            self._create_elements()
            self._create_component()

        self._connect_track_focus()
        self._request_identity()
        try:
            self.log_message('drumSeq | MPD32Sequencer control surface loaded')
        except (TypeError, AttributeError):
            pass

    # -- element construction --------------------------------------------
    def _create_elements(self):
        self._faders = [
            SliderElement(MIDI_CC_TYPE, CONTROL_CHANNEL, FADER_CC_BASE + i)
            for i in range(FADER_COUNT)]

        self._knobs = [
            EncoderElement(MIDI_CC_TYPE, CONTROL_CHANNEL, KNOB_CC_BASE + i,
                           MapMode.Absolute)
            for i in range(KNOB_COUNT)]

        self._pads = [
            ButtonElement(True, MIDI_NOTE_TYPE, NOTE_CHANNEL, PAD_NOTE_BASE + i)
            for i in range(PAD_NOTE_COUNT)]

        self._page_buttons = [
            ButtonElement(True, MIDI_CC_TYPE, CONTROL_CHANNEL, BUTTON_CC_BASE + i)
            for i in range(BUTTON_COUNT)]

    def _create_component(self):
        self._sequencer = SequencerDeviceComponent()
        self._sequencer.set_faders(self._faders)
        self._sequencer.set_knobs(self._knobs)
        self._sequencer.set_pads(self._pads)

        for index, button in enumerate(self._page_buttons):
            button.add_value_listener(self._make_page_handler(index))

    def _make_page_handler(self, index):
        def handler(value):
            if not value:
                return
            if index == 0:
                self._sequencer.set_page(self._sequencer._page - 1)
            elif index == 1:
                self._sequencer.set_page(self._sequencer._page + 1)
            elif index == 2:
                self._sequencer.set_page(0)
        return handler

    # -- DAW track/device focus ------------------------------------------
    def _get_song(self):
        attr = getattr(self, 'song', None)
        return attr() if callable(attr) else attr

    def _connect_track_focus(self):
        song = self._get_song()
        if song is None:
            return
        view = getattr(song, 'view', None)
        if view is None or not hasattr(view, 'add_selected_track_listener'):
            return
        try:
            view.add_selected_track_listener(self._on_selected_track_changed)
            self._focus_connected = True
        except (TypeError, AttributeError):
            return
        self._on_selected_track_changed()

    def _on_selected_track_changed(self):
        song = self._get_song()
        track = song.view.selected_track if song is not None else None
        device = find_drumseq_device(track)
        self._sequencer.set_device(device)
        name = device.name if device is not None else 'no sequencer focus'
        try:
            self.show_message('drumSeq | ' + name)
        except (TypeError, AttributeError):
            pass

    # -- USB handshake ----------------------------------------------------
    def _request_identity(self):
        if hasattr(self, 'schedule_message'):
            self.schedule_message(1, self._send_identity_request)

    def _send_identity_request(self):
        if hasattr(self, 'send_midi'):
            self.send_midi((0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7))
        if hasattr(self, 'schedule_message'):
            self.schedule_message(20, self._finalise_handshake)

    def handle_sysex(self, message):
        if identity_matches(message):
            self._identity_seen = True
            self.announce_connection()
        parent = getattr(super(MPD32Sequencer, self), 'handle_sysex', None)
        if callable(parent):
            parent(message)

    def _finalise_handshake(self):
        if not self._identity_seen:
            self._verified = True  # some MPD32 units do not answer identity
        self.announce_connection()

    def announce_connection(self):
        state = 'seen' if self._identity_seen else 'expected'
        try:
            self.show_message('Akai MPD32 <-> drumSeq profile (identity=%s)' % state)
        except (TypeError, AttributeError):
            pass

    def on_disconnect(self):
        try:
            song = self._get_song()
            if song is not None and self._focus_connected:
                view = getattr(song, 'view', None)
                if view is not None and hasattr(view, 'remove_selected_track_listener'):
                    view.remove_selected_track_listener(self._on_selected_track_changed)
        except (TypeError, AttributeError):
            pass
        self._sequencer.set_device(None)
        parent = getattr(super(MPD32Sequencer, self), 'on_disconnect', None)
        if callable(parent):
            parent()

    def disconnect(self):
        self.on_disconnect()
        parent = getattr(super(MPD32Sequencer, self), 'disconnect', None)
        if callable(parent):
            parent()


def create_instance(c_instance):
    """Factory entry point required by the Live MIDI Remote Script loader."""
    return MPD32Sequencer(c_instance=c_instance)