#!/bin/bash
# Builds the arm32 smoke harness against the armeabi-v7a libraries
# build-ffmpeg.sh produced, and runs it under qemu-arm.
#
# Run build-ffmpeg.sh first. Requires ANDROID_NDK_ROOT and a qemu-arm on PATH:
#   Ubuntu:  sudo apt-get install qemu-user-static
#   Then:    ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/29.0.14206865 bash arm32-smoke.sh
#
# armeabi-v7a is the one shipped ABI nothing here can execute: the maintainer
# owns no 32-bit device, and Apple silicon cannot run AArch32 at all, so a
# local emulator would be full-system emulation rather than a practical route.
# It was built four times over and never once RUN. This runs it.
#
# WHAT IT PROVES: the BUILD is sound for this ABI — the code executes, the ARM
# assembly is intact, the link is complete, and the codec set is really there
# at run time (the one check the README calls unfakeable). That is where this
# actually breaks: a lost configure flag, dropped assembly, a broken link.
#
# WHAT IT DOES NOT PROVE: anything about Android's dynamic loader — page
# alignment, SONAMEs, DT_NEEDED resolution inside the app's lib dir — or the
# JNI wiring, because the harness is linked STATICALLY. That is not a shortcut
# but the only option: qemu-user has no bionic dynamic linker to hand a
# shared-linked Android binary. Those properties are asserted for all four ABIs
# by build-ffmpeg.sh, and the JNI is one source file compiled per ABI, so what
# is left uncovered is small — closing it means running src/androidTest/ on a
# physical arm32 device (Firebase Test Lab), which is a separate decision.
#
# See src/test/cpp/arm32_smoke.c for what the harness checks.
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/prebuilt"
SMOKE="$DIR/prebuilt-static/armeabi-v7a"
HARNESS="$DIR/src/test/cpp/arm32_smoke.c"
# The same two synthetic fixtures the on-device test uses. Both are a
# generated tone and contain no recorded material; see the README beside them
# for the exact commands that produced them.
FIXTURES="$DIR/src/androidTest/assets"

# One source for the API floor: read it out of build-ffmpeg.sh rather than
# repeating it here, so a bumped floor cannot leave the harness compiled
# against a different API than the libraries it links.
API="$(sed -n 's/^API=\([0-9][0-9]*\)$/\1/p' "$DIR/build-ffmpeg.sh")"
if [ -z "$API" ]; then
  echo "ERROR: could not read the API level from build-ffmpeg.sh." >&2
  exit 1
fi

if [ -z "$ANDROID_NDK_ROOT" ]; then
  echo "ERROR: ANDROID_NDK_ROOT is not set." >&2
  exit 1
fi
if [ ! -d "$SMOKE" ]; then
  echo "ERROR: $SMOKE missing — run build-ffmpeg.sh first." >&2
  exit 1
fi

# Same host-tag detection as build-ffmpeg.sh: CI is linux-x86_64, a
# maintainer's Mac is darwin-x86_64. Hardcoding either makes this runnable in
# exactly one place.
HOST_TAG="$(ls "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt" | head -1)"
TOOLCHAIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG"
export PATH="$TOOLCHAIN/bin:$PATH"

QEMU="${QEMU_ARM:-qemu-arm-static}"
if ! command -v "$QEMU" >/dev/null 2>&1; then
  echo "ERROR: $QEMU not found. Install qemu-user-static, or set QEMU_ARM." >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
BIN="$WORK/arm32-smoke"

echo "NDK       : $ANDROID_NDK_ROOT"
echo "host tag  : $HOST_TAG"
echo "qemu      : $("$QEMU" --version | head -1)"
echo

# Static, and every library named explicitly in dependency order — a static
# link resolves left to right, so libavformat has to precede libavcodec, and
# libmp3lame has to follow it. -lz is the NDK's own: CONFIG_ZLIB is on and the
# mov demuxer reaches for it (compressed moov atoms).
"$TOOLCHAIN/bin/armv7a-linux-androideabi${API}-clang" \
  -static -Os -Wall -Wextra -Wno-unused-parameter -Werror \
  -o "$BIN" "$HARNESS" \
  -I"$OUT/include" \
  -L"$SMOKE" -lavformat -lavcodec -lswresample -lavutil -lmp3lame -lz -lm

# Two properties of the harness itself, so the log says what was run rather
# than leaving it to be assumed: it is a 32-bit ARM binary, and it carries no
# PT_INTERP (nothing is resolved at run time, so nothing about the loader is
# being tested here — see the note at the top).
file "$BIN" 2>/dev/null || llvm-readelf -h "$BIN" | sed -n '3,5p'
echo "PT_INTERP entries: $(llvm-readelf -l "$BIN" | grep -c INTERP || true)"
echo

# The one host-dependent thing in this script is the first instruction bionic
# executes: a 32-bit Android binary sets personality(PER_LINUX32) before main(),
# and an ARM64 kernel refuses that with EINVAL unless the CPU has 32-bit EL0 —
# which Apple silicon does not have at all. An x86_64 kernel accepts it (it is
# what `setarch --32bit` has always relied on), so CI is fine and an arm64
# workstation is not. Worth naming, because the abort happens before a single
# line of FFmpeg runs and therefore says nothing whatever about the build.
#
# No core dumps: on a signal, qemu-user writes a core of the whole guest
# address space to the cwd. The stack trace it would give is not something
# this job can act on anyway, and the file is large enough to be a nuisance.
ulimit -c 0
set +e
"$QEMU" "$BIN" "$FIXTURES" "$WORK" 2>&1 | tee "$WORK/smoke.log"
STATUS=${PIPESTATUS[0]}
set -e
if [ "$STATUS" -ne 0 ] && grep -q PER_LINUX32 "$WORK/smoke.log"; then
  echo >&2
  echo "NOTE: this host's kernel refused PER_LINUX32, so no 32-bit Android" >&2
  echo "      binary can start on it — nothing here was actually tested." >&2
  echo "      Run this on an x86_64 host (which is what CI is)." >&2
fi
exit "$STATUS"
