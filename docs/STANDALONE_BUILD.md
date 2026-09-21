# drumSeq Standalone — Universal (arm64 + x86_64) Build Guide

This guide covers the full macOS workflow for the **drumSeq Standalone**
application: Xcode project generation with universal slicing, the Release
build, ad-hoc deep code-signing, and local launch for verifying the internal
parametric drum synth engine and the virtual transport clock.

## 0. Prerequisites

| Tool        | Requirement                                                        |
| ----------- | ------------------------------------------------------------------ |
| macOS       | 14.4 or newer (matches `CMAKE_OSX_DEPLOYMENT_TARGET`)              |
| Xcode       | Installed with a Mac + iPhone runtimes (universal slices required) |
| Xcode CLT   | `xcode-select --install`                                           |
| JUCE        | Checkout at `<repo>/juce`, or point CMake at it via `-DJUCE_DIR=`  |
| CMake       | 3.22+ (`brew install cmake`)                                       |

```bash
# From the repository root
cd drumSeq

# JUCE lives next to the project by default (see CMakeLists.txt:12).
git submodule update --init         # if you keep JUCE as a submodule
# OR clone explicitly:
#   git clone --depth 1 https://github.com/juce-framework/JUCE.git juce
```

The `CMakeLists.txt` already hard-codes the universal architecture in
`CMAKE_OSX_ARCHITECTURES` (CMakeLists.txt:8), so the slicing instruction below
is belt-and-suspenders.

## 1. Generate the Xcode project (universal slices)

Run from the **repository root** (the source directory argument must point at
the directory containing `CMakeLists.txt`):

```bash
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" .
```

For a clean slate this is often combined with:

```bash
rm -rf build
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" .
```

If your JUCE checkout lives elsewhere:

```bash
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DJUCE_DIR=/path/to/JUCE .
```

> Note: the generator is multi-config, so `CMAKE_BUILD_TYPE` is ignored; the
> configuration is selected per-build (`Release` below).

## 2. Compile the Standalone target (Release)

```bash
cmake --build build --config Release --target drumSeq_Standalone
```

`juce_add_plugin (drumSeq ... FORMATS AU VST3 Standalone ...)` (CMakeLists.txt:22)
emits the standalone wrapper as the `drumSeq_Standalone` target and the app
bundle. With a multi-config Xcode generator the bundle lands at
`build/drumSeq_artefacts/Release/Standalone/drumSeq.app` (JUCE >= 7 layout);
`make_production_release.sh` auto-discovers this across generator layouts.

Parallel builds are safe with an Xcode generator:

```bash
cmake --build build --config Release --target drumSeq_Standalone -j "$(sysctl -n hw.ncpu)"
```

## 3. Verify both universal slices (optional but recommended)

```bash
lipo -info build/drumSeq_artefacts/Release/Standalone/drumSeq.app/Contents/MacOS/drumSeq
# Architectures in the fat file: build/drumSeq_artefacts/Release/Standalone/drumSeq.app/Contents/MacOS/drumSeq are: x86_64 arm64
```

## 4. Deep ad-hoc code-sign the app bundle

```bash
codesign --force --deep --sign - build/drumSeq_artefacts/Release/Standalone/drumSeq.app

# Confirm the signature:
codesign -dv build/drumSeq_artefacts/Release/Standalone/drumSeq.app 2>&1 | grep -E "Identifier|Signature"
```

## 5. Launch the standalone app

```bash
open build/drumSeq_artefacts/Release/Standalone/drumSeq.app
```

Once launched, the standalone wrapper has **no host transport** — the
`PluginAudioProcessor::processBlock` fallback branch ignites the internal
sample-aware virtual playhead clock. Drumming the sequencer pads schedules
note-ons that are routed straight into the pre-allocated `drumVoices`
parametric synth pool, accumulated sample-by-sample into the outbound
hardware buffer.

## Driver-channel safety notes

* The render pass clamps the write target to the channels the standalone
  audio device actually exposes (`juce::jlimit (0, 2, buffer.getNumChannels ())`) —
  a mono interface causes a safe 1-channel downmix, never an out-of-bounds
  write.
* Every synthesized sample is energy-clipped with `juce::jlimit (-1.0f, 1.0f)`
  after the 16-voice sum so driver buffers can never float-overflow.
* The virtual PPQ accumulator is derived solely from sample block length +
  sample rate; no `std::chrono` wall-clock queries run on the audio thread.

## One-shot automation harness

The repo also ships `build_mac.sh`, which does all of the above (plus lipo
verification, plugin install into `~/Library/Audio/Plug-Ins`, the Ableton
`MPD32Sequencer` remote-script provision and the zero-dependency unit tests):

```bash
./build_mac.sh --yes
```