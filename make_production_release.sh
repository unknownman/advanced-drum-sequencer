#!/usr/bin/env bash
#
# make_production_release.sh - drumSeq Universal Binary Production Freeze
#
# Principal DevOps build harness for the drumSeq *Standalone* application.
# End-to-end, self-contained, POSIX-safe (macOS zsh/bash):
#
#   1.  Environment audit       - verifies git, Xcode CLT, cmake, lipo,
#                                 codesign; auto-installs cmake via Homebrew.
#   2.  Dependency injection    - pre-seeds JUCE_DIR (defaults to
#                                 <repo>/juce), auto-clones JUCE 8.0.15 if the
#                                 configuration tree is missing. (8.0.0 cannot
#                                 compile on Xcode 16 + macOS 15 SDK.)
#   3.  Cross-compilation       - CMake + Xcode generator, universal slicing
#                                 enforced for BOTH arm64 and x86_64, macOS
#                                 14.4 deployment target (Task B matrix).
#   4.  Standalone build        - cmake --build --target drumSeq_Standalone.
#   5.  Slices audit            - lipo -info hard-fails unless BOTH slices
#                                 are present in the fat Mach-O.
#   6.  Deep ad-hoc codesigning  - codesign --force --deep --sign - to clear
#                                 macOS security/notarization flags.
#   7.  Verification launch      - open the .app so the parametric synth
#                                 engine / step playhead can be auditioned.
#   8.  Compilation metrics      - elapsed wall time + bundle size, logged
#                                 and echoed cleanly to stdout.
#
# Environment overrides (all optional):
#   CONFIG=Release|Debug|RelWithDebInfo     build configuration (default Release)
#   ARCHS='arm64;x86_64'                    universal slice matrix
#   DEPLOYMENT_TARGET=11.0                  macOS min-deploy OS
#   JUCE_VERSION=8.0.0                      JUCE release to auto-clone
#   JUCE_DIR=/path/to/juce                  explicit JUCE location
#   BUILD_DIR=/path/to/build                explicit CMake build directory
#   DO_LAUNCH=0                             skip the final `open` launch
#   DO_CLONE=0                              never auto-clone JUCE; fail instead
#
# Flags:
#   --no-launch   alias for DO_LAUNCH=0
#   --help        print usage and exit
#
set -uo pipefail
IFS=$'\n\t'

# ---------------------------------------------------------------------------
# Configuration (seeded from the environment; nothing hard-coded is required)
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd -- "$SCRIPT_DIR"

# Pre-seeded project-root dependency paths (Task A: exact JUCE_DIR injection).
JUCE_DIR="${JUCE_DIR:-$SCRIPT_DIR/juce}"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"

CONFIG="${CONFIG:-Release}"
ARCHS="${ARCHS:-arm64;x86_64}"
# Deployment-target floor: JUCE 8 needs >= 14.4 with Xcode 16 / the macOS 15
# SDK (CGWindowListCreateImage is obsoleted there). 8.0.0 does not compile on
# this toolchain; 8.0.15 is the patched release on the same 8.0 line whose
# window-snapshot path moved to ScreenCaptureKit.
DEPLOYMENT_TARGET="${DEPLOYMENT_TARGET:-14.4}"
JUCE_VERSION="${JUCE_VERSION:-8.0.15}"
JUCE_GIT_URL="${JUCE_GIT_URL:-https://github.com/juce-framework/JUCE.git}"

DO_LAUNCH="${DO_LAUNCH:-1}"
DO_CLONE="${DO_CLONE:-1}"

STANDALONE_TARGET="drumSeq_Standalone"
APP_BUNDLE=""
FREEZE_LOG="$BUILD_DIR/production_freeze.log"

T0="$(date +%s)"

log () { printf '[make_production_release] %s\n' "$*" | tee -a "$FREEZE_LOG"; }
warn () { printf '[WARN] %s\n' "$*" >&2; }
die  () { printf '[ERROR] %s\n' "$*" >&2; exit 1; }

usage () {
    sed -n '2,44p' "$0" | sed -E 's/^# ?//'
    exit 0
}

for arg in "$@"; do
    case "$arg" in
        --no-launch) DO_LAUNCH=0 ;;
        --help|-h)   usage ;;
        *) die "unknown argument: $arg" ;;
    esac
done

mkdir -p "$(dirname -- "$FREEZE_LOG")"

# ---------------------------------------------------------------------------
# Step 0: Native toolchain + OS environment audit
# ---------------------------------------------------------------------------
audit_tools () {
    log "Step 0/7: auditing native toolchain"

    command -v git       >/dev/null 2>&1 || die "git is required but not on PATH"
    command -v xcodebuild >/dev/null 2>&1 || die "Xcode is required but no xcodebuild on PATH"
    command -v lipo      >/dev/null 2>&1 || die "lipo (Xcode CLT) is missing"
    command -v codesign  >/dev/null 2>&1 || die "codesign (Security framework) is missing"

    if ! command -v cmake >/dev/null 2>&1; then
        log "cmake not found - attempting automated install via Homebrew"
        command -v brew >/dev/null 2>&1 || die "cmake missing and Homebrew is not installed; install cmake manually"
        brew install cmake || die "Homebrew failed to install cmake"
    fi

    log "compiler surface: $(cmake --version | head -n1)"
    log "Xcode location:   $(xcode-select -p)"

    # Universal slicing can only be honoured by an Apple LLVM (Xcode) toolchain,
    # so enforce that the active SDK deployment is Apple's.
    if ! xcodebuild -version >/dev/null 2>&1; then
        die "xcodebuild -version failed; ensure 'xcode-select --install' has completed"
    fi
}

# ---------------------------------------------------------------------------
# Step 1: Dependency injection - pre-seed JUCE or auto-clone (Task A)
# ---------------------------------------------------------------------------
ensure_juce () {
    log "Step 1/7: injecting JUCE dependency at $JUCE_DIR"

    if [ -f "$JUCE_DIR/CMakeLists.txt" ]; then
        log "JUCE configuration tree already present - reusing it"
        return 0
    fi

    if [ "$DO_CLONE" = "0" ]; then
        die "JUCE missing at '$JUCE_DIR' and DO_CLONE=0; clone it manually or set JUCE_DIR"
    fi

    log "JUCE not found at '$JUCE_DIR' - zero-dependency shallow clone (v$JUCE_VERSION)"
    git clone --depth 1 --branch "$JUCE_VERSION" --single-branch \
        "$JUCE_GIT_URL" "$JUCE_DIR" || die "failed to clone JUCE $JUCE_VERSION"

    [ -f "$JUCE_DIR/CMakeLists.txt" ] || die "JUCE clone succeeded but lacks CMakeLists.txt"
    log "JUCE $JUCE_VERSION ready: $(cd "$JUCE_DIR" && git describe --tags --always 2>/dev/null || echo shallow)"
}

# ---------------------------------------------------------------------------
# Step 2: CMake universal cross-compilation matrix (Task B, first stage)
# ---------------------------------------------------------------------------
configure () {
    log "Step 2/7: configuring Xcode generator, universal matrix $ARCHS"

    # Clean execution freeze: flush every prior CMake cache/generated-product
    # layer for the freeze build so no stale object or stale JUCE channel-bus
    # setting can leak into the archived universal binary.
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"

    # -DCMAKE_BUILD_TYPE is redundant with the Xcode multi-config generator but
    # kept for the documented production freeze matrix; the actual config is
    # selected per-build below with --config.
    cmake -B "$BUILD_DIR" \
          -G Xcode \
          -DCMAKE_BUILD_TYPE="$CONFIG" \
          -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
          -DJUCE_DIR="$JUCE_DIR" \
          "$SCRIPT_DIR" || die "cmake configuration failed"
}

# ---------------------------------------------------------------------------
# Locate the produced .app bundle across JUCE generator layouts.
# JUCE >= 7 (Xcode generator) emits the standalone at
#   <build>/drumSeq_artefacts/<Config>/Standalone/drumSeq.app
# whereas older JUCE runs emitted <build>/<Config>/drumSeq.app.
# ---------------------------------------------------------------------------
locate_app_bundle () {
    local candidate
    for candidate in \
        "$BUILD_DIR/drumSeq_artefacts/$CONFIG/Standalone/drumSeq.app" \
        "$BUILD_DIR/${STANDALONE_TARGET}_artefacts/$CONFIG/Standalone/drumSeq.app" \
        "$BUILD_DIR/$CONFIG/drumSeq.app"
    do
        if [ -d "$candidate" ] && [ -x "$candidate/Contents/MacOS/drumSeq" ]; then
            APP_BUNDLE="$candidate"
            APP_EXECUTABLE="$candidate/Contents/MacOS/drumSeq"
            return 0
        fi
    done

    local found
    found="$(find "$BUILD_DIR" -maxdepth 6 -type d -name 'drumSeq.app' -path '*Standalone*' 2>/dev/null | head -n1)"
    if [ -n "$found" ] && [ -x "$found/Contents/MacOS/drumSeq" ]; then
        APP_BUNDLE="$found"
        APP_EXECUTABLE="$found/Contents/MacOS/drumSeq"
        return 0
    fi
    return 1
}

# ---------------------------------------------------------------------------
# Step 3: Build the Standalone executable layer (Task B, second stage)
# ---------------------------------------------------------------------------
build_standalone () {
    log "Step 3/7: compiling '$STANDALONE_TARGET' ($CONFIG, $ARCHS)"

    cmake --build "$BUILD_DIR" --config "$CONFIG" --target "$STANDALONE_TARGET" \
        || die "standalone build failed"

    # Task 0 (a) requires the driver-agnostic resource client layer; the actual
    # app still routes through CoreAudio until the client is attached. The
    # bundle discovery below keeps archiving independent of the JUCE generator.
    locate_app_bundle || die "standalone build succeeded but the .app bundle could not be located"

    [ -x "$APP_EXECUTABLE" ] || die "expected executable '$APP_EXECUTABLE' was not produced"
    log "standalone executable present: $APP_EXECUTABLE"
}

# ---------------------------------------------------------------------------
# Step 4: Universal slices audit - hard gate on BOTH architecures (Task C)
# ---------------------------------------------------------------------------
lipo_audit () {
    log "Step 4/7: auditing universal binary slices"

    [ -f "$APP_EXECUTABLE" ] || die "app executable missing before lipo audit"

    lipo -info "$APP_EXECUTABLE"

    lipo -info "$APP_EXECUTABLE" | grep -q "arm64"  || die "arm64 slice is missing"
    lipo -info "$APP_EXECUTABLE" | grep -q "x86_64" || die "x86_64 slice is missing"
    log "PASS: fat binary carries BOTH arm64 and x86_64 slices"
}

# ---------------------------------------------------------------------------
# Step 5: Recursive ad-hoc deep codesign (Task C)
# ---------------------------------------------------------------------------
codesign_app () {
    log "Step 5/7: deep ad-hoc code signing"

    codesign --force --deep --sign - "$APP_BUNDLE" || die "codesign --deep failed"

    log "signature verdict: $(codesign -dv "$APP_BUNDLE" 2>&1 | grep -E 'Signature' | tr -d ' ')"
}

# ---------------------------------------------------------------------------
# Step 6: Compilation metrics (Task 3 expected output: clean tracking)
# ---------------------------------------------------------------------------
report_metrics () {
    local elapsed="$(($(date +%s) - T0))"
    local size="$(du -sh "$APP_BUNDLE" 2>/dev/null | cut -f1)"

    log "Step 6/7: metrics summary"
    log "  configuration  : $CONFIG"
    log "  architectures  : $ARCHS"
    log "  deployment OS  : macOS $DEPLOYMENT_TARGET"
    log "  JUCE           : $JUCE_VERSION @ $JUCE_DIR"
    log "  bundle         : $APP_BUNDLE (${size:-n/a})"
    log "  elapsed        : ${elapsed}s"
    log "  full log       : $FREEZE_LOG"
}

# ---------------------------------------------------------------------------
# Step 7: Verification launch (Task C) - audition standalone synth + playhead
# ---------------------------------------------------------------------------
launch_app () {
    if [ "$DO_LAUNCH" != "1" ]; then
        log "Step 7/7: launch skipped (DO_LAUNCH=0)"
        return 0
    fi

    log "Step 7/7: launching standalone app for audio verification"
    open "$APP_BUNDLE" || warn "open failed - launch manually: open $APP_BUNDLE"
    log "launched: open $APP_BUNDLE"
}

# ---------------------------------------------------------------------------
# Entry point - stages run in strict order; each aborts the freeze on failure
# ---------------------------------------------------------------------------
main () {
    audit_tools
    ensure_juce
    configure
    build_standalone
    lipo_audit
    codesign_app
    report_metrics
    launch_app

    log "PRODUCTION FREEZE COMPLETE ✅"
}

main "$@"