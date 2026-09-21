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
│  │  - APVTS 1233 params  │◄──►│  - fader/knob → VST param binding   │ │
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

### 1.4 Parametric internal synthesis engine

Standalone (and DAW fallback) audio is produced by a pre-allocated, 16-voice
internal drum synthesizer (`kDrumVoiceCount == 16`, round-robin voice-stealing)
that never touches the heap after `prepareToPlay()`. Every voice runs a
fully-polyphonic **exponential amplitude envelope** whose decay and release
stages drop along the logarithmic contour

```
amp(t) = e^(-t/τ),   τ = decay / release time constant (in seconds)
```

computed once per sample with a constant multiplier (`e^{-1/(τ·fs)}`) — **no
`exp()` is ever evaluated inside the render loop**, and the attack stage snaps
the transient then hands off to the decay curve toward the sustain level.

* **Lock-free LFO modulation** — each lane caches LFO rate, depth and waveform
  as `std::atomic<float>*` pointers (`SynthParamChannel`); the editor writes
  them through `SliderAttachment`/`ComboBoxAttachment` and the audio thread
  reads them with `memory_order_relaxed`. LFO phase advances per-sample against
  `lfo_rate / sampleRate` and modulates the oscillator pitch through the
  header-only `FastMathApproximations` sine (with triangle / sawtooth
  alternate planes) — zero allocations, zero locks.
* **Xorshift dark-noise colour filters** — a 32-bit xorshift PRNG drives the
  white-noise texture; each model then shapes the noise (first-order low-pass
  "darkening" coefficient derived from the lane's `noise_blend`; band-pass +
  180 Hz tone for the snare; high-pass for the hi-hat), so noise tails are
  colored and organic rather than brittle walls of white.
* **Model archetypes** — kick (sine-phase carrier with exponential pitch
  sweep), snare (band-passed noise + tone), hi-hat (high-passed noise); every
  voice also captures per-lane pitch, ADSR and blend parameters atomically at
  hit onset.

---

## 2. Repository Layout

```
drumSeq/
├── CMakeLists.txt                          # AU + VST3 + Standalone, universal binary
├── Source/
│   ├── PluginProcessor.h / .cpp            # lock-free engine + 1233-param APVTS
│   ├── PluginEditor.h / .cpp               # master grid, sidebar, bank selector
│   ├── DesignSystem/
│   │   ├── SequencerDesignSystem.h / .cpp  # pads, rotary, inc/dec look-and-feel
│   └── UI/
│       ├── DynamicSequencerPad.h / .cpp    # multi-action velocity pad + playhead dot
│       └── MidiDragExportComponent.h/.cpp  # OS-level drag-out MIDI export (.mid)
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
| macOS                           | 14.4+          | `CMAKE_OSX_DEPLOYMENT_TARGET=14.4`     |
| Xcode Command Line Tools        | 15.x+          | `xcode-select --install`               |
| CMake                           | 3.22+          | `brew install cmake`                   |
| JUCE                            | 8.0.15         | pinned (`git clone --branch 8.0.15 …`) |

### 3.2 One-time JUCE checkout

```bash
cd /Users/alijoder/Desktop/Code/VST/drumSeq
git clone --depth 1 --branch 8.0.15 https://github.com/juce-framework/JUCE.git juce
```

> JUCE is pinned to tag **8.0.15** for structural
> `AudioParameterFloatAttributes` compatibility; upstream JUCE 7.x is not a
> supported baseline for this build.

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

Generated products (Release, Xcode multi-config `drumSeq_artefacts` paths):

```
build/drumSeq_artefacts/Release/AU/drumSeq.component         (universal .component)
build/drumSeq_artefacts/Release/VST3/drumSeq.vst3             (universal .vst3)
build/drumSeq_artefacts/Release/Standalone/drumSeq.app        (universal .app)
```

Verify the binary actually contains both slices:

```bash
lipo -info build/drumSeq_artefacts/Release/VST3/drumSeq.vst3/Contents/MacOS/drumSeq
# -> Architectures in the fat file: ... are: x86_64 arm64
```

### 3.5 Ad-hoc codesign (local development)

macOS requires valid code signatures to load audio plug-ins:

```bash
codesign --force --deep --sign - build/drumSeq_artefacts/Release/AU/drumSeq.component
codesign --force --deep --sign - build/drumSeq_artefacts/Release/VST3/drumSeq.vst3
```

### 3.6 Install for Ableton Live

```bash
mkdir -p ~/Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/VST3
ditto build/drumSeq_artefacts/Release/AU/drumSeq.component ~/Library/Audio/Plug-Ins/Components/drumSeq.component
ditto build/drumSeq_artefacts/Release/VST3/drumSeq.vst3    ~/Library/Audio/Plug-Ins/VST3/drumSeq.vst3
```

Then restart Ableton Live (or rescan plug-ins) — `drumSeq` appears under
**Plug-ins → Drum**.

> Note: for distribution, replace the ad-hoc signature with an Apple
> Developer ID + notarization.

### 3.7 One-shot automation: build_mac.sh

`build_mac.sh` wraps §3.3–§3.6 + the Ableton remote-script install into a single
audited pipeline: dependency check, universal-slice `lipo` enforcement, ad-hoc
deep codesigning, and ditto install with platform prompts:

```bash
./build_mac.sh -y              # audit, configure, build, slice-verify, codesign, install
./build_mac.sh --no-install    # compile + verify only
JUCE_DIR=/path/to/JUCE ./build_mac.sh
```

It runs the zero-dependency Live remote-script suite automatically; run it
standalone any time:

```bash
python3 -m unittest discover -s Source/Tests -p 'test_*.py'
```

---

## 4. Hardware Integration Map

All mappings below mirror the engine constants (`PluginProcessor.h`) and the
remote script tables (`Source/Scripts/MPD32SequencerMap.py`).

### 4.1 Pads 1–16 (note assignments)

Pads send MIDI notes on **channel 1**. Each lane has four software note banks
`A/B/C/D` selectable from the editor header; the engine formula is
`note = 36 + lane + bank·16` (banks align to Ableton 16-pad Drum Racks).

| Pad | Lane  | Name        | Bank A | Bank B | Bank C | Bank D |
| --- | ----- | ----------- | ------ | ------ | ------ | ------ |
| 1   | 0     | Kick        | 36     | 52     | 68     | 84     |
| 2   | 1     | Snare       | 37     | 53     | 69     | 85     |
| 3   | 2     | Hat Closed  | 38     | 54     | 70     | 86     |
| 4   | 3     | Hat Open    | 39     | 55     | 71     | 87     |
| 5   | 4     | Clap        | 40     | 56     | 72     | 88     |
| 6   | 5     | Tom Low     | 41     | 57     | 73     | 89     |
| 7   | 6     | Tom Mid     | 42     | 58     | 74     | 90     |
| 8   | 7     | Tom High    | 43     | 59     | 75     | 91     |
| 9   | 8     | Rim         | 44     | 60     | 76     | 92     |
| 10  | 9     | Cowbell     | 45     | 61     | 77     | 93     |
| 11  | 10    | Shaker      | 46     | 62     | 78     | 94     |
| 12  | 11    | Claves      | 47     | 63     | 79     | 95     |
| 13  | 12    | Maracas     | 48     | 64     | 80     | 96     |
| 14  | 13    | Crash       | 49     | 65     | 81     | 97     |
| 15  | 14    | Ride        | 50     | 66     | 82     | 98     |
| 16  | 15    | Perc        | 51     | 67     | 83     | 99     |

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

The plug-in's `AudioProcessorValueTreeState` registers **1233 automatable,
host-visible parameters**, every one pre-cached to a `std::atomic<float>*` so
the audio thread never performs string lookups:

| Count | ID pattern                                   | Meaning                                  |
| ----- | -------------------------------------------- | ---------------------------------------- |
| 1     | `swing`                                      | swing amount 0..1                        |
| 512   | `lane_<lane>_step_<step>_vel`               | per-step velocity (normalized 0..1 = 0..127) |
| 512   | `lane_<lane>_step_<step>_prob`              | per-step trigger probability (0..1, gate in `renderLaneHit`) |
| 144   | `lane_<lane>_<synth_param>`                 | parametric synth: pitch, attack, decay, sustain, release, LFO rate/depth/wave, noise blend (9 × 16 lanes) |
| 64    | `lane_<lane>_euclidean_pulses` / `..._steps` | Björklund Euclidean macro pair per lane (32 × 2) |

The legacy velocity bus retains its exact layout: **index 0 is `swing`**, then
`1 + lane·32 + step` addressing over the 512 per-step velocity states (`lane`
0..15, `step` 0..31), as clocked per §1.2. In Live Script mode the 16 control
slots are paged over those parameters (`B1`/`B2`), slot `s` on page `p` mapping
to:

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