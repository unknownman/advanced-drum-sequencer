# drumSeq

A 16-lane polyrhythmic, hardware-first drum sequencer for **Ableton Live** as a
Universal Binary **VST3 / AU** plug-in, paired with a native **Akai MPD32**
control surface profile.

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Ableton Live                                                           │
│  ┌───────────────────────┐   ┌───────────────────────────────────────┐ │
│  │ drumSeq (VST3/AU)     │   │ MPD32Sequencer MIDI Remote Script     │ │
│  │  - lock-free 16×64 RT │   │  - MPD32 USB handshake (SysEx ID)     │ │
│  │  - APVTS 513 params   │◄──►│  - fader/knob → VST param binding   │ │
│  │  - Metal-backed UI    │   │  - selected-track focus routing       │ │
│  └───────────────────────┘   └───────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Repository Layout](#2-repository-layout)
3. [Mac Compilation Manual](#3-mac-compilation-manual)
4. [Hardware Integration Map](#4-hardware-integration-map)
5. [Ableton Live Routing Guide](#5-ableton-live-routing-guide)
6. [MIDI Learn & Per-Step Velocity](#6-midi-learn--per-step-velocity)

---

## 1. System Overview

### 1.1 Lock-free real-time engine

The sequencer core lives in `Source/PluginProcessor.{h,cpp}` and obeys the
hard real-time contract of the audio callback:

* **Zero allocation, zero locks, zero I/O, zero logging** inside
  `processBlock()`.
* UI → real-time hand-off uses pre-allocated, fixed-size
  `std::array<std::atomic<…>, N>` caches with `memory_order_relaxed` stores /
  loads (single-writer-per-field, atomic-by-nature values). No listener
  invokes or dispatches on the audio thread.
* No `notifyListeners`-driven repaints happen on the audio thread; the editor
  polls the atomics on a 30 Hz `juce::Timer`.

| Published atomic cache         | Type                          | Writer                | Reader                |
| ------------------------------ | ----------------------------- | --------------------- | --------------------- |
| `swingParamCache`              | `std::atomic<float>`          | UI param change       | RT `renderLaneHit`    |
| `fallbackBpmCache`             | `std::atomic<double>`         | RT `processBlock`     | RT (pre-playback BPM) |
| `targetNoteCaches[16]`         | `std::atomic<int>`            | UI / MIDI learn       | RT note trigger       |
| `loopLengthCaches[16]`         | `std::atomic<int>`            | UI / CC 22..29        | RT step addressing    |
| `velocityScaleCaches[16]`      | `std::atomic<float>`          | UI / CC 12..19        | RT velocity scale     |
| `currentStepCaches[16]`        | `std::atomic<int>`            | RT playhead           | UI playhead dot (30 Hz)|
| `noteBankCaches[4][16]`        | `std::atomic<int>`            | UI / MIDI learn       | `setActiveNoteBank`   |
| `stepVelocityCaches[16][64]`   | `std::atomic<float>`          | UI (APVTS listener)   | RT step activation    |
| `laneInLearnMode`              | `std::atomic<int>`            | UI / CC 32..39        | RT MIDI learn capture |

### 1.2 Polyrhythmic clock mathematics

Each lane runs an independent loop length (`1..64`), all clocked by the host
transport. A 16th-note is expressed in samples as

```
samplesPerStep = (60.0 · sampleRate · 0.25) / bpm
```

* The scheduler (`scheduleLanes`) turns `ppqStart` into a fractional 16th
  position and emits every 16th boundary that falls inside the current block,
  with an exact-boundary + front-edge discontinuity guard so Live gives glitch
  free note placement at loop starts and after seeks.
* Swing: lanes with an even `laneStep` fire on the grid; odd steps are delayed
  by `samplesPerStep · swing · 0.5` (`swing` in `0..1`), giving classic
  "shuffle" feel on odd subdivisions.
* Active steps are deduplicated per lane via `lastSteps[]`, and a short
  `note-off` (~80 ms) is scheduled after every `note-on`.

### 1.3 Metal-backed vector interface

`Source/DesignSystem/SequencerDesignSystem.*` implements the full look-and-feel
(`LookAndFeel_V4`) for pads, rotaries and inc/dec steppers. The Mac build
targets JUCE's native rendering pipeline, which composites on Apple silicon
through the **Metal** GPU backend; all geometry is emitted as `juce::Path`
vectors, so the 512-pad master grid (16 lanes × 32 steps) and its playhead
tracking repaint are GPU-composited with no texture churn.

---

## 2. Repository Layout

```
drumSeq/
├── CMakeLists.txt                          # AU + VST3 + Standalone, universal binary
├── Source/
│   ├── PluginProcessor.h / .cpp            # lock-free engine + 513-param APVTS
│   ├── PluginEditor.h / .cpp               # master grid, sidebar, bank selector
│   ├── DesignSystem/
│   │   ├── SequencerDesignSystem.h / .cpp  # pads, rotary, inc/dec look-and-feel
│   └── UI/
│       ├── DynamicSequencerPad.h / .cpp    # multi-action velocity pad + playhead dot
│   └── Scripts/
│       ├── __init__.py                     # Ableton Live MPD32Sequencer ControlSurface
│       └── MPD32SequencerMap.py            # hardware↔VST parameter tables (pure Python)
├── README.md
```

---

## 3. Mac Compilation Manual

### 3.1 Prerequisites

| Tool                            | Version        | Notes                                  |
| ------------------------------- | -------------- | -------------------------------------- |
| macOS                           | 11.0+          | `CMAKE_OSX_DEPLOYMENT_TARGET=11.0`     |
| Xcode Command Line Tools        | 15.x+          | `xcode-select --install`               |
| CMake                           | 3.22+          | `brew install cmake`                   |
| JUCE                            | 7.x / 8.x      | `git clone --depth 1 --branch 8.0.0 …` |

### 3.2 One-time JUCE checkout

```bash
cd /Users/alijoder/Desktop/Code/VST/drumSeq
git clone --depth 1 --branch 8.0.0 https://github.com/juce-framework/JUCE.git juce
```

> Use any JUCE 7/8 tag; the build only needs the JUCE **library** (no Projucer).

### 3.3 Configure a Universal Binary (arm64 + x86_64) Xcode project

```bash
cmake -B build \
      -G Xcode \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DJUCE_DIR="$(pwd)/juce"
```

> `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` builds **one** Universal Binary
> containing both architectures.

### 3.4 Build

```bash
cmake --build build --config Release
```

Generated products (default CMake/JUCE paths):

```
build/Release/drumSeq.component
build/Release/drumSeq.vst3
build/Release/drumSeq.app
```

Verify the binary actually contains both slices:

```bash
lipo -info build/Release/drumSeq.vst3/Contents/MacOS/drumSeq
# -> Architectures in the fat file: build/Release/drumSeq.vst3/Contents/MacOS/drumSeq are: x86_64 arm64
```

### 3.5 Ad-hoc codesign (local development)

macOS requires valid code signatures to load audio plug-ins:

```bash
codesign --force --deep --sign - build/Release/drumSeq.component
codesign --force --deep --sign - build/Release/drumSeq.vst3
```

### 3.6 Install for Ableton Live

```bash
mkdir -p ~/Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/VST3
ditto build/Release/drumSeq.component ~/Library/Audio/Plug-Ins/Components/drumSeq.component
ditto build/Release/drumSeq.vst3    ~/Library/Audio/Plug-Ins/VST3/drumSeq.vst3
```

Then restart Ableton Live (or rescan plug-ins) — `drumSeq` appears under
**Plug-ins → Drum**.

> Note: for distribution, replace the ad-hoc signature with an Apple
> Developer ID + notarization.

---

## 4. Hardware Integration Map

All mappings below mirror the engine constants (`PluginProcessor.h`) and the
remote script tables (`Source/Scripts/MPD32SequencerMap.py`).

### 4.1 Pads 1–16 (note assignments)

Pads send MIDI notes on **channel 1**. Each lane has four software note banks
`A/B/C/D` selectable from the editor header; the engine formula is
`note = 36 + lane + bank·12`.

| Pad | Lane  | Name        | Bank A | Bank B | Bank C | Bank D |
| --- | ----- | ----------- | ------ | ------ | ------ | ------ |
| 1   | 0     | Kick        | 36     | 48     | 60     | 72     |
| 2   | 1     | Snare       | 37     | 49     | 61     | 73     |
| 3   | 2     | Hat Closed  | 38     | 50     | 62     | 74     |
| 4   | 3     | Hat Open    | 39     | 51     | 63     | 75     |
| 5   | 4     | Clap        | 40     | 52     | 64     | 76     |
| 6   | 5     | Tom Low     | 41     | 53     | 65     | 77     |
| 7   | 6     | Tom Mid     | 42     | 54     | 66     | 78     |
| 8   | 7     | Tom High    | 43     | 55     | 67     | 79     |
| 9   | 8     | Rim         | 44     | 56     | 68     | 80     |
| 10  | 9     | Cowbell     | 45     | 57     | 69     | 81     |
| 11  | 10    | Shaker      | 46     | 58     | 70     | 82     |
| 12  | 11    | Claves      | 47     | 59     | 71     | 83     |
| 13  | 12    | Maracas     | 48     | 60     | 72     | 84     |
| 14  | 13    | Crash       | 49     | 61     | 73     | 85     |
| 15  | 14    | Ride        | 50     | 62     | 74     | 86     |
| 16  | 15    | Perc        | 51     | 63     | 75     | 87     |

### 4.2 Faders, knobs & control buttons

All controls are **MIDI CC, channel 1**. Two operational modes exist:

* **Raw CC mode** — the MPD32's control data is routed straight into the
  plug-in track (Monitor **In**). The engine's `processHardwareController()`
  consumes the CCs directly, lock-free, in the audio thread.
* **Live Script mode** — the `MPD32Sequencer` remote script intercepts the
  CCs and drives the plug-in through **host parameter automation** (the
  recommended setup; see §5).

| Control | CC  | Raw CC mode (engine)                    | Live Script mode (VST param)       |
| ------- | --- | --------------------------------------- | ---------------------------------- |
| F1      | 12  | velocity trim lane 0                    | page 0 → `swing` (param 0)         |
| F2…F8   | 13–19| velocity trim lanes 1–7 (bank 0) / 8–15 (bank 1) | page slots 1–7 |
| K1…K8   | 22–29| loop length lanes 0–7 (bank 0) / 8–15 (bank 1)   | page slots 8–15   |
| B1      | 32  | toggle MIDI-learn lane 0                | page −1                           |
| B2      | 33  | toggle MIDI-learn lane 1                | page +1                           |
| B3      | 34  | toggle MIDI-learn lane 2                | reset to page 0                   |
| B4…B8   | 35–39| toggle MIDI-learn lanes 3–7             | reserved                          |

### 4.3 VST parameter index model

The plug-in's `AudioProcessorValueTreeState` registers **513 automatable
parameters** in exact order:

| Param index | ID                         | Meaning                          |
| ----------- | -------------------------- | -------------------------------- |
| 0           | `swing`                    | swing amount 0..1               |
| `1 + lane·32 + step` | `lane_<lane>_step_<step>_vel` | per-step velocity (0..127, exposed as 0..1) |

In Live Script mode the 16 control slots are paged over those parameters
(`B1`/`B2`), slot `s` on page `p` mapping to:

```
page 0:     s == 0 → param 0 (swing),  else → param s
page p ≥ 1: → param p·16 + s
```

(The same formula is implemented in `MPD32SequencerMap.control_param_index()`.)

---

## 5. Ableton Live Routing Guide

### 5.1 Install the MPD32Sequencer control surface profile

```bash
mkdir -p "/Applications/Ableton Live 11 Suite.app/Contents/App-Resources/MIDI Remote Scripts/MPD32Sequencer"
cp Source/Scripts/__init__.py          "/Applications/Ableton Live 11 Suite.app/Contents/App-Resources/MIDI Remote Scripts/MPD32Sequencer/"
cp Source/Scripts/MPD32SequencerMap.py "/Applications/Ableton Live 11 Suite.app/Contents/App-Resources/MIDI Remote Scripts/MPD32Sequencer/"
```

Then:

1. Restart Ableton Live.
2. **Live → Preferences → Link, Tempo & MIDI**.
3. **Control Surface:** choose `MPD32Sequencer`, set **Input** = *MPD32 In*,
   **Output** = *MPD32 Out*.

The script performs a USB handshake (Universal SysEx identity probe), shows
*"Akai MPD32 <-> drumSeq profile"* in the status bar and keeps hardware bound
to whichever track currently has focus.

### 5.2 Track routing — sequence into a Drum Rack

1. Create a **MIDI track**; load **drumSeq** as a plug-in on it.
2. Set the track's **MIDI From** input to *MPD32* (pads) — or leave it *All
   Ins* if you only compose from the sequencer grid.
3. Set **Monitor** to **In** so the *16th-note pattern* and live pad hits are
   both audible immediately.
4. Create a second MIDI track with a **Drum Rack** of your choosing and set its
   **MIDI From** to *drumSeq* (the plug-in's generated notes are delivered on
   track output channel 1).
5. Arm the Drum Rack track; adjust the 16 lane notes in Panel §4.1 to match
   the rack's pad mapping (Drag/Drop the racks to audition).

Optionally drop an **Ableton MIDI Track → Mixer** and set the drumSeq track's
TCI (Timebase) so Live's transport clocks the polyrhythmic scheduler exactly.

### 5.3 Choosing raw CC vs Live script mode

* Use **Live Script mode** when you want faders/knobs to automate the plug-in
  parameters from the *arrangement/clip automation* lanes and you are only
  feeding pads into Live (recommended default).
* Use **Raw CC mode** when no remote script is installed and the whole MIDI
  stream (pads + CCs) is routed directly to the drumSeq track — note that the
  engine's built-in CC decoder (`processHardwareController`) then takes over
  velocity trims, loop lengths and MIDI-learn toggles.

---

## 6. MIDI Learn & Per-Step Velocity

* **MIDI learn (per lane):** enable the magenta **LRN** button on a track row
  (or press `B1..B8` in raw CC mode), then hit any hardware pad/note. The
  assignment stores into the active note bank and survives bank switches.
* **Step programming:** click a pad to arm it (velocity 100); vertical drag
  adjusts its velocity from 1–127. The pad paints a bottom-up gradient meter
  for the velocity, and a magenta playhead dot tracks the running step.
* **Bank switching:** the header's **A/B/C/D** selector swaps all 16 lane note
  assignments in one shot (four 16-note drum maps per project).

---

## Validation & Off-boarding

* All real-time state crosses the audio boundary only through pre-sized atomics
  (§1.1) — the scheduler kernel is allocation-free.
* `Source/Scripts/MPD32SequencerMap.py` is pure Python and is unit-tested
  standalone (`python3 -m py_compile` + contract assertions) against the C++
  constants; both remote-script modules target Live 10 (legacy `_Framework`)
  and Live 11/12 (`ableton.v2`) — Python 2/3 safe.
* Generated behind a Universal Binary: `lipo -info` verifies `x86_64 arm64`.