#!/usr/bin/env bash
#
# build_mac.sh - drumSeq Universal Binary build, audit, code-sign & install.
#
# Production automation harness for the AU/VST3/Standalone build documented in
# README.md. Responsibilities:
#   1. Dependency audit (tools, Xcode toolchain, JUCE checkout, path hygiene)
#   2. Xcode project configuration enforcing "arm64;x86_64" + macOS 11.0
#   3. Compile, then verifies BOTH universal slices with lipo (hard failure)
#   4. Ad-hoc deep codesigning of the .component and .vst3 trees
#   5. Install into ~/Library/Audio/Plug-Ins (Components + VST3)
#   6. Provision the MPD32Sequencer remote script into Ableton Live 11/12
#
# Options:
#   -y, --yes         answer every prompt with "yes"
#   --no-install      skip the plug-in directory install step
#   --no-codesign     skip ad-hoc codesigning
#   --no-tests        skip the zero-dependency remote-script unit tests
#   -c, --config NAME release (default) | debug | relwithdebinfo
#
# Environment overrides: JUCE_DIR, BUILD_DIR, CONFIG.
set -uo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd -- "$SCRIPT_DIR"

CONFIG="${CONFIG:-Release}"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"
JUCE_DIR="${JUCE_DIR:-$SCRIPT_DIR/juce}"
DEPLOYMENT_TARGET="11.0"
ARCHS="arm64;x86_64"

PLUGIN_COMPONENTS_DIR="$HOME/Library/Audio/Plug-Ins/Components"
PLUGIN_VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
REMOTE_SCRIPT_NAME="MPD32Sequencer"

DO_INSTALL=1
DO_CODESIGN=1
DO_TESTS=1
ASSUME_YES=0

# ---------------------------------------------------------------------------
# Communicative plumbing
# ---------------------------------------------------------------------------

die() { printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }
warn() { printf '\033[1;33m[WARN]\033[0m %s\n' "$*" >&2; }
info() { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m    %s\033[0m\n' "$*"; }
plain() { printf '    %s\n' "$*"; }

confirm_or_default() {
    if [ "$ASSUME_YES" = "1" ]; then
        return 0
    fi
    local question="$1"
    local answer
    while true; do
        printf '%s [Y/n] ' "$question"
        read -r answer || return 1
        case "${answer:-Y}" in
            [Yy]|[Yy]es) return 0 ;;
            [Nn]|[Nn]o) return 1 ;;
            *) plain 'Please answer y or n.' ;;
        esac
    done
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || die "Required tool '$1' was not found on PATH."
}

usage() {
    echo "Usage: $0 [-y|--yes] [--no-install] [--no-codesign] [--no-tests]"
    echo "              [-c|--config Release] [JUCE_DIR=/path ./$0]"
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

while [ "$#" -gt 0 ]; do
    case "$1" in
        -y|--yes) ASSUME_YES=1 ;;
        --no-install) DO_INSTALL=0 ;;
        --no-codesign) DO_CODESIGN=0 ;;
        --no-tests) DO_TESTS=0 ;;
        -c|--config)
            [ "$#" -ge 2 ] || die "Option '$1' requires an argument."
            CONFIG="$2"
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        *) die "Unknown option '$1'. See --help." ;;
    esac
    shift
done

case "$(uname -s)" in
    Darwin) ;;
    *) die "build_mac.sh is macOS-only (running on: $(uname -s))." ;;
esac

# ---------------------------------------------------------------------------
# 0/6 dependency audit
# ---------------------------------------------------------------------------

info "Configuring production build"
plain "- product: drumSeq (AU + VST3 + Standalone)"
plain "- config: $CONFIG"
plain "- build:  $BUILD_DIR"
plain ""

for tool in cmake xcodebuild lipo codesign ditto python3; do
    require_tool "$tool"
done
ok "Core toolchain present: cmake, xcodebuild, lipo, codesign, ditto, python3."

DEV_DIR="$(xcode-select -p 2>/dev/null || true)"
if [ -z "$DEV_DIR" ] || [ ! -d "$DEV_DIR" ]; then
    die "No active Xcode developer directory. Run 'sudo xcode-select -s /Applications/Xcode.app'."
fi
ok "Xcode developer directory: $DEV_DIR"

CMAKE_VERSION="$(cmake --version 2>/dev/null | head -n 1)"
ok "CMake: $CMAKE_VERSION"

# ---------------------------------------------------------------------------
# JUCE library audit (Prompt Task B1)
# ---------------------------------------------------------------------------

if [ ! -f "$JUCE_DIR/CMakeLists.txt" ]; then
    warn "JUCE library not found at: $JUCE_DIR"
    plain "The CMake project requires a JUCE 7/8 checkout at this location."
    plain "  git clone --depth 1 --branch 8.0.0 https://github.com/juce-framework/JUCE.git \"$JUCE_DIR\""
    plain "  or re-run with  JUCE_DIR=/path/to/JUCE ./build_mac.sh"
    if confirm_or_default "Clone JUCE 8.0.0 into $JUCE_DIR automatically?"; then
        require_tool git
        rm -rf "$JUCE_DIR" || true
        git clone --depth 1 --branch 8.0.0 https://github.com/juce-framework/JUCE.git "$JUCE_DIR" \
            || die "JUCE clone failed. Provide the checkout or set JUCE_DIR."
        ok "JUCE cloned into $JUCE_DIR"
    else
        die "JUCE is required to build. Configure the path via JUCE_DIR and re-run."
    fi
fi
ok "JUCE library found: $JUCE_DIR"

REPO_ROOT_MARKER="$SCRIPT_DIR/CMakeLists.txt"
[ -f "$REPO_ROOT_MARKER" ] || die "Script must stay at the repository root (missing CMakeLists.txt)."

# ---------------------------------------------------------------------------
# 1/6 configure Xcode project (Prompt Tasks B2 & C architecture enforcement)
# ---------------------------------------------------------------------------

info "Configuring Xcode project (architectures: ${ARCHS}, deployment target: ${DEPLOYMENT_TARGET})"

cmake -B "$BUILD_DIR" -G Xcode \
      -DCMAKE_BUILD_TYPE="$CONFIG" \
      -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
      -DJUCE_DIR="$JUCE_DIR" \
    || die "CMake configuration failed. Inspect the output above for cross-compilation errors."

# Verify the enforced settings actually landed in the generated cache.
CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
if [ -f "$CACHE_FILE" ]; then
    grep -Eq '^CMAKE_OSX_ARCHITECTURES:STRING=.*arm64.*x86_64.*' "$CACHE_FILE" \
        || die "Architecture enforcement failed: CMAKE_OSX_ARCHITECTURES not set to '$ARCHS' in CMakeCache.txt."
    grep -Eq "^CMAKE_OSX_DEPLOYMENT_TARGET:STRING=$DEPLOYMENT_TARGET\$" "$CACHE_FILE" \
        || die "Deployment target enforcement failed: CMAKE_OSX_DEPLOYMENT_TARGET is not '$DEPLOYMENT_TARGET'."
fi
ok "CMake configuration complete; universal ('$ARCHS') + macOS $DEPLOYMENT_TARGET enforced."

# ---------------------------------------------------------------------------
# 2/6 compile (Prompt Task B2)
# ---------------------------------------------------------------------------

info "Compiling drumSeq ($CONFIG) with xcodebuild"
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel \
    || die "Compilation failed. Architecture cross-compilation errors would appear above."

STAGE="$BUILD_DIR/$CONFIG"
VST3_TREE="$STAGE/drumSeq.vst3"
AU_TREE="$STAGE/drumSeq.component"
APP_TREE="$STAGE/drumSeq.app"
VST3_BIN="$VST3_TREE/Contents/MacOS/drumSeq"

[ -d "$VST3_TREE" ] || die "Expected VST3 bundle missing after build: $VST3_TREE"
[ -d "$AU_TREE" ] || die "Expected AU bundle missing after build: $AU_TREE"

# ---------------------------------------------------------------------------
# 3/6 universal slice audit (Prompt Task B3 - explicit failure state)
# ---------------------------------------------------------------------------

info "Auditing universal binary slices with lipo"
if [ ! -f "$VST3_BIN" ]; then
    die "Built VST3 binary not found: $VST3_BIN"
fi

ARCH_LIST="$(lipo -archs "$VST3_BIN" 2>/dev/null || \
    die "lipo could not read the architecture list from: $VST3_BIN")"
plain "lipo -archs -> $ARCH_LIST"

case " $ARCH_LIST " in
    *" arm64 "* ) ;;
    * ) die "UNIVERSAL SLICE AUDIT FAILED: arm64 slice missing from $VST3_BIN (got: $ARCH_LIST)." ;;
esac
case " $ARCH_LIST " in
    *" x86_64 "* ) ;;
    * ) die "UNIVERSAL SLICE AUDIT FAILED: x86_64 slice missing from $VST3_BIN (got: $ARCH_LIST)." ;;
esac

lipo -info "$VST3_BIN" 2>/dev/null | sed 's/^/    /'
ok "Universal binary verified: both 'arm64' and 'x86_64' slices present."

# ---------------------------------------------------------------------------
# 4/6 zero-dependency verification (Prompt Task A)
# ---------------------------------------------------------------------------

if [ "$DO_TESTS" = "1" ]; then
    info "Running zero-dependency remote-script verification suite"
    python3 -m unittest discover -s "$SCRIPT_DIR/Source/Tests" -p 'test_*.py' -v \
        || die "Remote-script verification suite FAILED."
    ok "All remote-script verification tests passed."
fi

# ---------------------------------------------------------------------------
# 5/6 ad-hoc deep codesign (Prompt Task B4)
# ---------------------------------------------------------------------------

if [ "$DO_CODESIGN" = "1" ]; then
    info "Ad-hoc deep codesigning bundles"
    codesign --force --deep --sign - "$AU_TREE" \
        || die "codesign failed on: $AU_TREE"
    codesign --force --deep --sign - "$VST3_TREE" \
        || die "codesign failed on: $VST3_TREE"
    ok "Deep-ad-hoc signed $AU_TREE and $VST3_TREE"

    codesign --verify --strict --deep "$AU_TREE" \
        || warn "codesign --verify reported problems for $AU_TREE"
    codesign --verify --strict --deep "$VST3_TREE" \
        || warn "codesign --verify reported problems for $VST3_TREE"
fi

# ---------------------------------------------------------------------------
# 6/6 install & Ableton Live provisioning (Prompt Task C)
# ---------------------------------------------------------------------------

if [ "$DO_INSTALL" = "1" ]; then
    info "Installing into user plug-in libraries"
    if confirm_or_default "Install drumSeq AU + VST3 into ~/Library/Audio/Plug-Ins?"; then
        mkdir -p "$PLUGIN_COMPONENTS_DIR" "$PLUGIN_VST3_DIR"
        ditto "$AU_TREE" "$PLUGIN_COMPONENTS_DIR/drumSeq.component" \
            || die "ditto failed while installing the .component"
        ditto "$VST3_TREE" "$PLUGIN_VST3_DIR/drumSeq.vst3" \
            || die "ditto failed while installing the .vst3"
        ok "Installed ~/Library/Audio/Plug-Ins/Components/drumSeq.component"
        ok "Installed ~/Library/Audio/Plug-Ins/VST3/drumSeq.vst3"
    else
        warn "Skipped plug-in installation."
    fi
fi

info "Provisioning the Ableton Live remote script"

LIVE_CANDIDATES="$(ls -d /Applications/Ableton\ Live*.app 2>/dev/null || true)"
if [ -z "$LIVE_CANDIDATES" ]; then
    warn "No Ableton Live installation found under /Applications; skipping remote-script provisioning."
else
    # Prefer the newest supported major version (12 over 11), but only accept
    # apps whose Info.plist declares a major version of 11 or 12.
    LIVE_TARGET=""
    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        major="$(defaults read "$candidate/Contents/Info" CFBundleShortVersionString 2>/dev/null \
                 | cut -d. -f1 || true)"
        case "$major" in
            11|12) LIVE_TARGET="$candidate"; break ;;
        esac
    done < <(printf '%s\n' "$LIVE_CANDIDATES" | LC_ALL=C sort -V -r)

    if [ -z "$LIVE_TARGET" ]; then
        warn "Ableton Live found, but none is version 11 or 12; skipping remote-script provisioning."
    else
        SCRIPT_DEST="$LIVE_TARGET/Contents/App-Resources/MIDI Remote Scripts/$REMOTE_SCRIPT_NAME"
        SCRIPT_SRC="$SCRIPT_DIR/Source/Scripts"
        info "Targeting: $LIVE_TARGET"
        if confirm_or_default "Copy Source/Scripts into $SCRIPT_DEST?"; then
            for required in __init__.py MPD32SequencerMap.py; do
                [ -f "$SCRIPT_SRC/$required" ] || die "Remote-script source missing: $SCRIPT_SRC/$required"
            done
            mkdir -p "$SCRIPT_DEST"
            ditto "$SCRIPT_SRC/" "$SCRIPT_DEST/" \
                || die "ditto failed provisioning the remote script into: $LIVE_TARGET"
            python3 -m py_compile "$SCRIPT_DEST/__init__.py" "$SCRIPT_DEST/MPD32SequencerMap.py" \
                || die "Installed remote script failed py_compile - provisioning aborted."

            if [ -d "$SCRIPT_DEST/__pycache__" ]; then
                rm -rf "$SCRIPT_DEST/__pycache__"
            fi

            ok "Provisioned Ableton Live remote script -> $SCRIPT_DEST"
            plain "Restart Live and select 'MPD32Sequencer' under Preferences > MIDI > Control Surface."
        else
            warn "Skipped Ableton Live remote-script provisioning."
        fi
    fi
fi

ok "build_mac.sh complete: build, slice audit, codesign and provisioning finished."