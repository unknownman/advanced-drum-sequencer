#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Zero-dependency verification harness for the drumSeq Ableton remote script.

Simulates the exact framework surface Ableton Live exposes to MIDI Remote
Scripts - ``Live.MidiMap.MapMode``, ``_Framework``'s ``ControlSurface``,
``EncoderElement`` / ``SliderElement`` / ``ButtonElement`` and the
``InputControlElement`` type constants - using only the Python standard
library (no Ableton, no third-party packages).

The *real* ``Source/Scripts`` package is copied into a throwaway package
directory (mirroring installation into
``App-Resources/MIDI Remote Scripts/MPD32Sequencer/``) and imported against
the mocks, so every code path below executes the actual files - proving the
script loads with zero syntax errors or attribute failures inside the DAW.

Run::

    python3 -m unittest discover -s Source/Tests -p 'test_*.py'
    python3 Source/Tests/test_remote_script.py
"""

from __future__ import absolute_import
from __future__ import print_function

import contextlib
import os
import shutil
import sys
import tempfile
import types
import unittest

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
SCRIPTS_DIR = os.path.normpath(os.path.join(TESTS_DIR, os.pardir, 'Scripts'))

_PACKAGE_NAME = 'MPD32Sequencer'
_MOCK_MODULES = ('Live', 'Live.MidiMap', 'Live.MidiMap.MapMode', '_Framework')

TMP_ROOT = None
SCRIPT = None
MAP = None


# ---------------------------------------------------------------------------
# 1. Live framework mocks (pure stdlib)
# ---------------------------------------------------------------------------

class MapMode(object):
    Absolute = 0
    absolute = 0


class FrameworkElement(object):
    """Baseline behaviour shared by Live Encoder/Slider/Button elements."""

    def __init__(self, *args, **kwargs):
        self._listeners = []
        self.sent_values = []

    def add_value_listener(self, callback):
        if callback not in self._listeners:
            self._listeners.append(callback)

    def remove_value_listener(self, callback):
        if callback in self._listeners:
            self._listeners.remove(callback)

    def send_value(self, value, channel=None):
        self.sent_values.append(value)


class EncoderElement(FrameworkElement):
    def __init__(self, *args, **kwargs):
        super(EncoderElement, self).__init__(*args, **kwargs)


class SliderElement(FrameworkElement):
    def __init__(self, *args, **kwargs):
        super(SliderElement, self).__init__(*args, **kwargs)


class ButtonElement(FrameworkElement):
    def __init__(self, *args, **kwargs):
        super(ButtonElement, self).__init__(*args, **kwargs)


class ControlSurface(object):
    """Minimal stand-in for _Framework.ControlSurface.ControlSurface."""

    def __init__(self, c_instance=None):
        self._c_instance = c_instance
        self.song = getattr(c_instance, 'song', None) if c_instance is not None else None
        self.sent_midi = []
        self.scheduled = []
        self.messages = []

    @contextlib.contextmanager
    def component_guard(self):
        yield

    def send_midi(self, message):
        if self._c_instance is not None and hasattr(self._c_instance, 'send_midi'):
            self._c_instance.send_midi(message)
        self.sent_midi.append(message)

    def schedule_message(self, ticks, callback):
        if self._c_instance is not None and hasattr(self._c_instance, 'schedule_message'):
            self._c_instance.schedule_message(ticks, callback)
        self.scheduled.append((ticks, callback))

    def show_message(self, text):
        self.messages.append(('show', text))
        if self._c_instance is not None and hasattr(self._c_instance, 'show_message'):
            self._c_instance.show_message(text)

    def log_message(self, text):
        self.messages.append(('log', text))
        if self._c_instance is not None and hasattr(self._c_instance, 'log_message'):
            self._c_instance.log_message(text)

    def handle_sysex(self, message):
        pass

    def on_disconnect(self):
        pass

    def disconnect(self):
        pass


class Component(object):
    """Stand-in for _Framework.ControlSurfaceComponent / ableton ControlSurface Component."""

    def __init__(self, *args, **kwargs):
        self.update_callback = None


class InputControlElement(object):
    MIDI_CC_TYPE = 1
    MIDI_NOTE_TYPE = 2


# ---------------------------------------------------------------------------
# 2. Absent-real-object mocks (Live device object model + C-interface)
# ---------------------------------------------------------------------------

class FakeParameter(object):
    def __init__(self, name, value=0.5):
        self.name = name
        self.value = value
        self._listeners = []

    def add_value_listener(self, callback):
        if callback not in self._listeners:
            self._listeners.append(callback)

    def remove_value_listener(self, callback):
        if callback in self._listeners:
            self._listeners.remove(callback)


def make_confirmed_parameters():
    """513 live parameters named exactly like the C++ layout (index == name)."""
    parameters = [FakeParameter(MAP.param_name(0), value=0.5)]
    for lane in range(MAP.LANE_COUNT):
        for step in range(MAP.AUTOMATED_STEP_COUNT):
            index = MAP.step_velocity_param_index(lane, step)
            parameters.append(FakeParameter(MAP.param_name(index), value=0.0))
    return parameters


class FakeDevice(object):
    def __init__(self, name='drumSeq', parameters=None):
        self.class_name = 'drumSeq'
        self.name = name
        self.parameters = parameters if parameters is not None else []


class FakeTrack(object):
    def __init__(self, device):
        self.devices = [device]


class FakeSongView(object):
    def __init__(self, device):
        self.selected_track = FakeTrack(device)
        self._listeners = []

    def add_selected_track_listener(self, callback):
        if callback not in self._listeners:
            self._listeners.append(callback)

    def remove_selected_track_listener(self, callback):
        if callback in self._listeners:
            self._listeners.remove(callback)


class FakeSong(object):
    def __init__(self, device):
        self.view = FakeSongView(device)


class CInstance(object):
    """Dummy C-interface context container handed to create_instance()."""

    def __init__(self, device=None):
        self.song = FakeSong(device if device is not None else FakeDevice())
        self.events = []

    def send_midi(self, message):
        self.events.append(('send_midi', message))

    def schedule_message(self, ticks, callback):
        self.events.append(('schedule_message', ticks, callback))

    def show_message(self, text):
        self.events.append(('show_message', text))

    def log_message(self, text):
        self.events.append(('log_message', text))


# ---------------------------------------------------------------------------
# 3. Mock install + real-file import
# ---------------------------------------------------------------------------

def _install_module(name, namespace):
    module = types.ModuleType(name)
    for key, value in namespace.items():
        setattr(module, key, value)
    sys.modules[name] = module
    return module


def install_live_mocks():
    live = _install_module('Live', {})
    midimap = _install_module('Live.MidiMap', {})
    midimap.MapMode = MapMode
    midimap.MapMode.absolute = MapMode.absolute
    live.MidiMap = midimap

    _install_module('Live.MidiMap.MapMode',
                    {'Absolute': MapMode.Absolute, 'absolute': MapMode.Absolute})

    framework = types.ModuleType('_Framework')
    framework.__path__ = []
    sys.modules['_Framework'] = framework

    _install_module('_Framework.ControlSurface', {'ControlSurface': ControlSurface})
    _install_module('_Framework.ControlSurfaceComponent',
                    {'ControlSurfaceComponent': Component})
    _install_module('_Framework.EncoderElement',
                    {'EncoderElement': EncoderElement, 'MapMode': MapMode})
    _install_module('_Framework.SliderElement', {'SliderElement': SliderElement})
    _install_module('_Framework.ButtonElement', {'ButtonElement': ButtonElement})
    _install_module('_Framework.InputControlElement',
                    {'MIDI_CC_TYPE': InputControlElement.MIDI_CC_TYPE,
                     'MIDI_NOTE_TYPE': InputControlElement.MIDI_NOTE_TYPE})


def import_scripts():
    """Copy the real Scripts/ folder into a temp package and import it."""
    global TMP_ROOT
    TMP_ROOT = tempfile.mkdtemp(prefix='drumseq_remote_script_')
    pkg_dir = os.path.join(TMP_ROOT, _PACKAGE_NAME)
    os.makedirs(pkg_dir)
    shutil.copy2(os.path.join(SCRIPTS_DIR, '__init__.py'),
                 os.path.join(pkg_dir, '__init__.py'))
    shutil.copy2(os.path.join(SCRIPTS_DIR, 'MPD32SequencerMap.py'),
                 os.path.join(pkg_dir, 'MPD32SequencerMap.py'))
    sys.path.insert(0, TMP_ROOT)
    __import__(_PACKAGE_NAME)
    return sys.modules[_PACKAGE_NAME]


def setUpModule():
    global SCRIPT, MAP
    install_live_mocks()
    SCRIPT = import_scripts()
    MAP = SCRIPT.MPD32SequencerMap


def tearDownModule():
    global TMP_ROOT
    if TMP_ROOT is None:
        return
    if TMP_ROOT in sys.path:
        sys.path.remove(TMP_ROOT)
    for name in list(sys.modules):
        if name == _PACKAGE_NAME or name.startswith(_PACKAGE_NAME + '.'):
            del sys.modules[name]
    for name in _MOCK_MODULES:
        sys.modules.pop(name, None)
    shutil.rmtree(TMP_ROOT, ignore_errors=True)
    TMP_ROOT = None


# ---------------------------------------------------------------------------
# 4. Tests
# ---------------------------------------------------------------------------

class ContractTests(unittest.TestCase):
    def test_map_contract_wellformed(self):
        self.assertEqual(MAP.PARAM_COUNT, 513)
        self.assertEqual(MAP.ENGINE_NOTE_BANK_BASE, (36, 52, 68, 84))
        self.assertEqual(MAP.LANE_COUNT, 16)
        self.assertEqual(MAP.AUTOMATED_STEP_COUNT, 32)
        self.assertEqual(MAP.param_name(0), 'Swing')
        self.assertEqual(MAP.param_name(1), 'Lane 1 Step 1 Velocity')
        self.assertEqual(MAP.param_name(33), 'Lane 2 Step 1 Velocity')
        self.assertEqual(MAP.param_name(512), 'Lane 16 Step 32 Velocity')
        self.assertIsNone(MAP.param_name(513))

    def test_identity_matches_handshake(self):
        match = SCRIPT.identity_matches
        self.assertTrue(
            match((0xF0, 0x7E, 0x00, 0x06, 0x02, 0x00, 0x00, 0x65, 0x00, 0x00, 0xF7)))
        self.assertFalse(
            match((0xF0, 0x7E, 0x00, 0x06, 0x02, 0x00, 0x00, 0x66, 0x00, 0x00, 0xF7)))
        self.assertFalse(
            match((0xF0, 0x7E, 0x00, 0x06, 0x02, 0x00, 0x00, 0xF7)))
        self.assertFalse(match('not-a-tuple'))


class ConstructionTests(unittest.TestCase):
    def test_create_instance_with_dummy_c_context(self):
        device = FakeDevice(parameters=make_confirmed_parameters())
        context = CInstance(device=device)
        surface = SCRIPT.create_instance(c_instance=context)

        self.assertIsInstance(surface, SCRIPT.ControlSurface)
        self.assertEqual(len(surface._faders), MAP.FADER_COUNT)
        self.assertEqual(len(surface._knobs), MAP.KNOB_COUNT)
        self.assertEqual(len(surface._pads), MAP.PAD_NOTE_COUNT)
        self.assertEqual(len(surface._page_buttons), MAP.BUTTON_COUNT)

        self.assertTrue(surface._focus_connected)
        self.assertIs(surface._sequencer._device, device)
        self.assertTrue(surface._sequencer._element_listeners)

        self.assertGreaterEqual(len(surface.scheduled), 1)
        self.assertFalse(surface._verified)

        self.assertTrue(any(getattr(element, 'sent_values', None)
                            for element in surface._pads))

    def test_handle_sysex_identity(self):
        context = CInstance()
        surface = SCRIPT.create_instance(c_instance=context)
        surface.handle_sysex(
            (0xF0, 0x7E, 0x00, 0x06, 0x02, 0x00, 0x00, 0x65, 0x00, 0x00, 0xF7))
        self.assertTrue(surface._identity_seen)
        self.assertTrue(any(message[0] == 'show_message' and 'MPD32' in message[1]
                            for message in context.events))


class NamedResolutionTests(unittest.TestCase):
    def test_device_parameter_resolves_by_name(self):
        device = FakeDevice(parameters=make_confirmed_parameters())
        self.assertIs(SCRIPT._device_parameter(device, 0), device.parameters[0])
        self.assertEqual(SCRIPT._device_parameter(device, 0).name, 'Swing')
        self.assertEqual(SCRIPT._device_parameter(device, 1).name, 'Lane 1 Step 1 Velocity')
        self.assertEqual(SCRIPT._device_parameter(device, 33).name, 'Lane 2 Step 1 Velocity')
        self.assertEqual(SCRIPT._device_parameter(device, 512).name, 'Lane 16 Step 32 Velocity')

    def test_device_parameter_prefers_name_over_index(self):
        parameters = make_confirmed_parameters()
        parameters[0], parameters[1] = parameters[1], parameters[0]
        device = FakeDevice(parameters=parameters)
        self.assertEqual(SCRIPT._device_parameter(device, 0).name, 'Swing')
        self.assertEqual(SCRIPT._device_parameter(device, 1).name, 'Lane 1 Step 1 Velocity')

    def test_device_parameter_positional_fallback(self):
        unnamed = [FakeParameter('omit%d' % i, value=0.5) for i in range(6)]
        device = FakeDevice(parameters=unnamed)
        self.assertIs(SCRIPT._device_parameter(device, 2), unnamed[2])
        self.assertIsNone(SCRIPT._device_parameter(device, 40))

    def test_find_parameter_by_name_misses(self):
        device = FakeDevice(parameters=make_confirmed_parameters())
        self.assertIsNotNone(SCRIPT.find_parameter_by_name(device, 'Swing'))
        self.assertIsNone(SCRIPT.find_parameter_by_name(device, 'Not A Param'))
        self.assertIsNone(SCRIPT.find_parameter_by_name(None, 'Swing'))


class ListenLifecycleTests(unittest.TestCase):
    def _make_wired_component(self):
        component = SCRIPT.SequencerDeviceComponent()
        component.set_faders([SliderElement() for _ in range(MAP.FADER_COUNT)])
        component.set_knobs([EncoderElement() for _ in range(MAP.KNOB_COUNT)])
        component.set_pads([ButtonElement() for _ in range(MAP.PAD_NOTE_COUNT)])
        return component

    @staticmethod
    def _elements(component):
        return list(component._controls) + list(component._pads)

    def test_set_device_detach_reattach(self):
        component = self._make_wired_component()
        device_a = FakeDevice(parameters=make_confirmed_parameters())
        device_b = FakeDevice(name='drumSeq B', parameters=make_confirmed_parameters())

        component.set_device(device_a)
        elements = self._elements(component)
        self.assertEqual(len(component._element_listeners), 32)
        self.assertEqual(len(component._param_listeners), 32)
        self.assertTrue(all(len(element._listeners) == 1 for element in elements))
        # Page 0 binds params 0..15, referenced once per control AND once per pad.
        self.assertTrue(all(len(param._listeners) == 2 for param in device_a.parameters[:16]))
        self.assertTrue(all(len(param._listeners) == 0 for param in device_a.parameters[16:32]))

        component.set_device(None)
        self.assertEqual(component._element_listeners, [])
        self.assertEqual(component._param_listeners, [])
        self.assertTrue(all(len(element._listeners) == 0 for element in elements))
        self.assertTrue(all(len(param._listeners) == 0 for param in device_a.parameters[:32]))

        component.set_device(device_b)
        self.assertEqual(len(component._element_listeners), 32)
        self.assertTrue(all(len(element._listeners) == 1 for element in elements))
        self.assertTrue(all(len(param._listeners) == 0 for param in device_a.parameters[:32]))
        self.assertTrue(all(len(param._listeners) == 2 for param in device_b.parameters[:16]))

    def test_set_page_force_feedback_rebind(self):
        component = self._make_wired_component()
        component.set_device(FakeDevice(parameters=make_confirmed_parameters()))
        component.set_page(3)
        self.assertEqual(component._page, 3)
        self.assertIs(component._element_listeners[0][0], component._controls[0])
        self.assertGreaterEqual(len(component._element_listeners), 32)

    def test_parameter_write_normalisation(self):
        component = self._make_wired_component()
        param = FakeParameter('Swing', value=0.5)
        component._set_parameter(param, 64)
        self.assertAlmostEqual(param.value, 64 / 127.0, places=3)
        component._set_parameter(param, 127)
        self.assertAlmostEqual(param.value, 1.0, places=3)
        component._set_parameter(None, 10)


if __name__ == '__main__':
    unittest.main()