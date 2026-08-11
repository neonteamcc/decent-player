#!/bin/bash
# Builds an audio-only FFmpeg (+ libmp3lame) for the four Android ABIs.
#
# Run setup.sh first. Requires ANDROID_NDK_ROOT, e.g.
#   ANDROID_NDK_ROOT=~/Library/Android/sdk/ndk/29.0.14206865 bash build-ffmpeg.sh
#
# Output: prebuilt/<abi>/lib/lib{avcodec,avformat,avutil,swresample}.so
#         prebuilt/include/**    (headers, shared by all ABIs)
#         prebuilt/COPYING.*     (the licence notices these libraries ship under)
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/src/main/jni/upstream"
OUT="$DIR/prebuilt"

# API 28 matches the app's minSdk. A library floor above it would ship a
# download pipeline that some supported devices cannot load at all.
API=28

if [ -z "$ANDROID_NDK_ROOT" ]; then
  echo "ERROR: ANDROID_NDK_ROOT is not set." >&2
  exit 1
fi
if [ ! -d "$SRC/ffmpeg" ] || [ ! -d "$SRC/lame" ]; then
  echo "ERROR: upstream sources missing — run setup.sh first." >&2
  exit 1
fi

# The NDK ships its host prebuilts under a host tag. CI is linux-x86_64; a
# maintainer's Mac is darwin-x86_64 (Apple silicon runs it under Rosetta).
# Hardcoding either one makes the script runnable in exactly one place.
HOST_TAG="$(ls "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt" | head -1)"
TOOLCHAIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG"
# nproc is Linux-only; sysctl is the macOS equivalent.
JOBS="$( (nproc 2>/dev/null) || sysctl -n hw.ncpu )"

echo "NDK       : $ANDROID_NDK_ROOT"
echo "host tag  : $HOST_TAG"
echo "jobs      : $JOBS"

# ---------------------------------------------------------------------------
# No host path may appear in a configure argument.
#
# ffmpeg's configure stores its ENTIRE argument list in FFMPEG_CONFIGURATION
# (config.h), and every library carries that string at runtime as
# avcodec_configuration(). An absolute --prefix/--cc/--sysroot therefore ends
# up inside the shipped .so files, which breaks two things at once:
#
#   1. These artifacts are published from a PUBLIC repository. Baking in
#      /Users/<name>/... or /home/<name>/... leaks the build machine's home
#      directory, and with it the maintainer's username.
#   2. Reproducibility. The same commit built on a Mac and on a CI runner
#      would produce byte-different libraries purely because the paths differ.
#
# So: $TOOLCHAIN/bin goes on PATH and every tool is named bare. NDK clang
# locates its own sysroot relative to itself, so --sysroot is unnecessary
# too. --prefix is a fixed fictional path and `make install DESTDIR=`
# relocates the result. libmp3lame is staged INSIDE the ffmpeg source tree so
# that ffmpeg can find it through a relative -I/-L (configure and make both
# run with the ffmpeg source root as cwd, so the relative form resolves).
#
# The "leak scan" at the end asserts this on the finished artifacts — it is
# the check that must never be removed, because the failure is invisible
# until someone runs `strings` on a published library.
# ---------------------------------------------------------------------------
export PATH="$TOOLCHAIN/bin:$PATH"
NEUTRAL_PREFIX="/decent-audio-ffmpeg"

# Android 15+ devices may use 16 KB memory pages. NDK r28+ links with a
# 16 KB max-page-size by default, but we state it explicitly because these
# .so files are produced by upstream configure scripts, not by the NDK's
# own build systems — nothing else here would guarantee it.
PAGE_LDFLAGS="-Wl,-z,max-page-size=16384"

# The components the download pipeline actually needs. Asserted against the
# generated config_components.h right after each configure — see
# assert_components() for why that file and not the binary's strings.
REQUIRED_COMPONENTS="
  MOV_DEMUXER FLAC_DEMUXER MP3_DEMUXER AAC_DEMUXER WAV_DEMUXER
  FLAC_MUXER MP4_MUXER IPOD_MUXER MP3_MUXER WAV_MUXER
  FLAC_DECODER AAC_DECODER MP3_DECODER EAC3_DECODER
  PCM_S16LE_DECODER PCM_S24LE_DECODER PCM_F32LE_DECODER
  FLAC_ENCODER LIBMP3LAME_ENCODER PCM_S16LE_ENCODER PCM_S24LE_ENCODER
  FLAC_PARSER AAC_PARSER MPEGAUDIO_PARSER
  FILE_PROTOCOL
"
# Negative controls. These are not merely "not needed" — they are the exact
# codecs whose descriptor long_names ARE present in the binary regardless
# (see assert_components), so asserting they are 0 is what proves
# --disable-everything actually held.
FORBIDDEN_COMPONENTS="H264_DECODER ALAC_DECODER OPUS_DECODER VORBIS_ENCODER"

# Asserts the codec set on the generated config_components.h, which is
# ffmpeg's own authoritative record of what configure enabled.
#
# Do NOT be tempted to check the built .so instead:
#   * `nm -D | grep ff_flac_encoder` returns 0 even on a correct build —
#     ff_* codec structs are hidden symbols and the version script limits the
#     dynamic symbol table to the public av* API.
#   * grepping the codecs' long_name strings is worse, because it looks like
#     it works. libavcodec/codec_desc.c compiles a descriptor for EVERY
#     codec unconditionally, so "FLAC (Free Lossless Audio Codec)" is in the
#     strings of a --disable-everything build that has no FLAC encoder at
#     all. Measured on this exact configuration: "H.264 / AVC" 1,
#     "Apple Lossless Audio Codec" 1, "Opus" 1 — none of which this build can
#     decode. A build that silently lost --enable-encoder=flac would pass.
#     ("PCM signed 24-bit little-endian" additionally substring-matches its
#     own ...planar variant.)
# Must run after configure and before the next ABI's `make distclean`, which
# deletes config_components.*.
assert_components() {
  local ABI="$1" SYM BAD=0
  for SYM in $REQUIRED_COMPONENTS; do
    grep -q "^#define CONFIG_$SYM 1\$" config_components.h || {
      echo "  MISSING  CONFIG_$SYM" >&2; BAD=1; }
  done
  for SYM in $FORBIDDEN_COMPONENTS; do
    grep -q "^#define CONFIG_$SYM 0\$" config_components.h || {
      echo "  UNEXPECTEDLY ENABLED  CONFIG_$SYM" >&2; BAD=1; }
  done
  # CONFIG_LIBMP3LAME is the external library itself and lives in config.h,
  # not config_components.h. Without it the mp3 encoder silently is not there.
  grep -q '^#define CONFIG_LIBMP3LAME 1$' config.h || {
    echo "  MISSING  CONFIG_LIBMP3LAME (config.h)" >&2; BAD=1; }
  if [ "$BAD" -ne 0 ]; then
    echo "ERROR: $ABI configure did not produce the required codec set." >&2
    exit 1
  fi
  echo "codec set OK ($ABI): $(echo $REQUIRED_COMPONENTS | wc -w | tr -d ' ') components + libmp3lame"
}

build_abi() {
  local ABI=$1 TRIPLE=$2 CPU=$3
  local PREFIX="$OUT/$ABI"
  local CC="${TRIPLE}${API}-clang"
  # libmp3lame is staged inside the ffmpeg tree so ffmpeg can reference it
  # relatively. DEPS_REL is what goes on ffmpeg's command line; the absolute
  # form is only ever used by LAME itself, whose paths are not recorded
  # anywhere in the shipped libraries.
  local DEPS_REL="deps/$ABI"
  local DEPS_ABS="$SRC/ffmpeg/$DEPS_REL"

  # ffmpeg's distclean runs FIRST, before LAME is staged into deps/. Doing it
  # the other way round would put a `rm` of the previous config in between
  # staging the dependency and using it.
  ( cd "$SRC/ffmpeg" && { make distclean >/dev/null 2>&1 || true; } )
  rm -rf "$DEPS_ABS"

  # ffmpeg's x86 SIMD is written in nasm syntax, so configure hard-fails on
  # x86/x86_64 unless nasm is installed on the HOST. We disable it instead of
  # adding a host prerequisite, on purpose and unconditionally — gating it on
  # "is nasm present" would make the produced binaries depend on the machine
  # that built them. The cost is confined to the two ABIs that are emulators
  # and a handful of x86 Chromebooks, on a file-conversion path that is not
  # latency-critical; arm64-v8a and armeabi-v7a keep all their assembly.
  #
  # 32-bit x86 additionally needs --disable-inline-asm: libavcodec/x86/
  # lpc_init.c (the FLAC encoder's autocorrelation) emits R_386_32 against a
  # local symbol, which cannot be linked into a shared object, so the build
  # dies at LD with "recompile with -fPIC" even though -fPIC is already on.
  # That ABI therefore falls back to plain C, which is fine for it.
  local ASMFLAGS=""
  case "$CPU" in
    x86_64) ASMFLAGS="--disable-x86asm" ;;
    x86)    ASMFLAGS="--disable-x86asm --disable-inline-asm" ;;
  esac

  echo
  echo "=== lame $ABI ==="
  # LAME is built STATIC on purpose: it is an implementation detail folded
  # into libavcodec.so, and libavcodec.so itself stays shared (see README).
  # It installs into the ffmpeg tree's deps/, never into the published
  # prefix — so libmp3lame.a and lame/*.h cannot reach prebuilt/ at all and
  # nothing downstream can link LAME a second time by accident.
  #
  # CFLAGS is repeated as a make-level override, and that is not redundant.
  # LAME's configure appends its own "OPTIMIZATION" flags derived from the
  # HOST cpu even in a cross build; for host_cpu=i686 that includes
  # -mtune=native, which the NDK clang resolves against the machine running
  # the build and rejects ("unknown target CPU 'apple-m4'"). Overriding
  # CFLAGS on the make command line drops the whole host-derived set, so all
  # four ABIs compile with exactly the flags we chose.
  ( cd "$SRC/lame" && { make distclean >/dev/null 2>&1 || true; }
    ./configure --host="$TRIPLE" --prefix="$DEPS_ABS" \
      --disable-shared --enable-static --disable-frontend --disable-decoder \
      --disable-dependency-tracking \
      CC="$CC" AR="llvm-ar" RANLIB="llvm-ranlib" \
      NM="llvm-nm" STRIP="llvm-strip" \
      CFLAGS="-Os -fPIC"
    make -j"$JOBS" CFLAGS="-Os -fPIC" && make install CFLAGS="-Os -fPIC" )

  echo
  echo "=== ffmpeg $ABI ==="
  # SONAMEs: Android's packager only extracts files matching lib*.so, so a
  # default libavcodec.so.62 would never reach a device. No overrides are
  # needed here — ffmpeg's own `--target-os=android` case already sets
  #   SLIB_INSTALL_NAME='$(SLIBNAME)'  SLIB_INSTALL_LINKS=
  #   SHFLAGS='-shared -Wl,-soname,$(SLIBNAME)'
  # i.e. it installs one unversioned file with an unversioned SONAME.
  # Verified after the build with llvm-readelf -d; do not "restore" manual
  # SLIBNAME= overrides — configure rejects unknown VAR= arguments outright.
  #
  # Every argument below is host-neutral by construction; see the block at
  # the top of this file before adding one that is not.
  local STAGE="$OUT/.stage-$ABI"
  rm -rf "$STAGE"
  ( cd "$SRC/ffmpeg"
    ./configure \
      --prefix="$NEUTRAL_PREFIX" \
      --target-os=android --arch="$CPU" --enable-cross-compile \
      --cc="$CC" --cross-prefix="llvm-" --nm="llvm-nm" \
      --enable-shared --disable-static \
      --disable-everything --disable-programs --disable-doc --disable-avdevice \
      --disable-swscale --disable-avfilter --disable-network \
      --disable-iconv --disable-symver \
      --enable-demuxer=mov,flac,mp3,aac,wav \
      --enable-muxer=flac,mp4,ipod,mp3,wav \
      --enable-decoder=flac,aac,mp3,eac3,pcm_s16le,pcm_s24le,pcm_f32le \
      --enable-encoder=flac,libmp3lame,pcm_s16le,pcm_s24le \
      --enable-parser=flac,aac,mpegaudio \
      --enable-protocol=file \
      --enable-libmp3lame \
      --extra-cflags="-Os -fPIC -I$DEPS_REL/include" \
      --extra-ldflags="-L$DEPS_REL/lib $PAGE_LDFLAGS" \
      --build-suffix="" \
      --enable-pic \
      $ASMFLAGS
    assert_components "$ABI"
    make -j"$JOBS" && make install DESTDIR="$STAGE" )

  # Relocate the neutral prefix into the published per-ABI tree.
  mkdir -p "$PREFIX"
  mv "$STAGE$NEUTRAL_PREFIX"/* "$PREFIX/"
  rm -rf "$STAGE"

  # pkgconfig: the .pc files record `prefix=` as an absolute path, and there
  # is no consumer for them — Task 2's CMake links these libraries by
  # explicit path, not through pkg-config. Dropping them removes a whole
  # class of path leakage rather than sanitising it.
  rm -rf "$PREFIX/lib/pkgconfig"
  # share/: --disable-doc does not stop `make install` from copying ffmpeg's
  # 26 example .c files, and they would be published per ABI.
  rm -rf "$PREFIX/share"
}

rm -rf "$OUT"
build_abi arm64-v8a   aarch64-linux-android    aarch64
build_abi armeabi-v7a armv7a-linux-androideabi arm
build_abi x86_64      x86_64-linux-android     x86_64
build_abi x86         i686-linux-android       x86

# Task 2's CMake wants ONE include tree, not four. The installed headers are
# ABI-independent in practice (avconfig.h only records endianness/alignment
# traits that all four ABIs share), but assert it rather than assume it — and
# assert it fatally: publishing the arm64 tree after a divergence would hand
# Task 2 headers that do not describe three of the four libraries it links.
echo
echo "=== publishing shared headers ==="
for ABI in armeabi-v7a x86_64 x86; do
  if ! diff -r "$OUT/arm64-v8a/include" "$OUT/$ABI/include" >/tmp/ffmpeg-hdr-diff.$$ 2>&1; then
    echo "ERROR: headers differ between arm64-v8a and $ABI:" >&2
    cat /tmp/ffmpeg-hdr-diff.$$ >&2
    rm -f /tmp/ffmpeg-hdr-diff.$$
    exit 1
  fi
  rm -f /tmp/ffmpeg-hdr-diff.$$
done
cp -R "$OUT/arm64-v8a/include" "$OUT/include"
echo "headers identical across all four ABIs; published prebuilt/include"

# These libraries are shared precisely so that the LGPL's relinking
# requirement is satisfied by construction. The licence text is the other
# half of that obligation, so it travels with the binaries.
cp "$SRC/ffmpeg/COPYING.LGPLv2.1" "$OUT/COPYING.ffmpeg.LGPLv2.1"
cp "$SRC/lame/COPYING"            "$OUT/COPYING.lame.LGPLv2"

echo
echo "=== verify ==="
# A versioned SONAME is the failure this build exists to avoid: the library
# would package and then simply not be there at dlopen() time. Assert it on
# every ABI rather than spot-checking one by hand.
FAIL=0
for ABI in arm64-v8a armeabi-v7a x86_64 x86; do
  for LIB in libavcodec libavformat libavutil libswresample; do
    SO="$OUT/$ABI/lib/$LIB.so"
    if [ ! -f "$SO" ]; then
      echo "MISSING  $ABI/$LIB.so"; FAIL=1; continue
    fi
    SONAME="$(llvm-readelf -d "$SO" \
      | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p')"
    if [ "$SONAME" != "$LIB.so" ]; then
      echo "BAD SONAME  $ABI/$LIB.so -> '$SONAME'"; FAIL=1
    fi
  done
done
# Note the `if` rather than `&&`: under `set -e` a failing `&&` list at top
# level aborts the script, which would skip the checks and the summary below.
if [ "$FAIL" -eq 0 ]; then echo "SONAMEs OK (all unversioned, 4 ABIs x 4 libs)"; fi

# The codec set was already asserted per ABI on config_components.h, right
# after each configure. What is left to prove on the artifact is that
# libmp3lame really was folded in, which its own banner does show: unlike a
# codec long_name, "LAME3.100" comes from the LAME sources, not from
# ffmpeg's unconditional descriptor table.
LAME_HITS="$(llvm-strings "$OUT/arm64-v8a/lib/libavcodec.so" | grep -cF "LAME3.100" || true)"
echo "libmp3lame folded into arm64-v8a/libavcodec.so: LAME3.100 x $LAME_HITS"
[ "$LAME_HITS" -gt 0 ] || FAIL=1

# Leak scan. These artifacts are published from a PUBLIC repository, and
# ffmpeg bakes its whole configure line into every library, so a single
# absolute path in an argument ships the build machine's home directory.
# It also makes the build unreproducible across hosts. Asserted, not trusted.
echo
echo "=== leak scan (host paths in a public artifact) ==="
for SO in "$OUT"/*/lib/*.so; do
  N="$(llvm-strings "$SO" | grep -cF "$HOME" || true)"
  printf "  %-46s \$HOME x %s\n" "${SO#$OUT/}" "$N"
  [ "$N" -eq 0 ] || FAIL=1
done
# Wider than the .so files: catches headers, and any .pc file that a future
# change stops deleting. $DIR and $TOOLCHAIN are checked separately from
# $HOME so the scan still works if the repo or the NDK sits outside it.
#
# -a is load-bearing. Without it this leg silently skips every binary, and
# it does so INCONSISTENTLY across hosts: GNU grep -l still lists a matching
# binary, but a ugrep installed as `grep` (common on developer Macs) does
# not. Forcing text mode makes the result the same everywhere.
for NEEDLE in "$HOME" "$DIR" "$TOOLCHAIN"; do
  HITS="$(grep -ralF -- "$NEEDLE" "$OUT" 2>/dev/null || true)"
  if [ -n "$HITS" ]; then
    echo "LEAK: '$NEEDLE' appears in published files:" >&2
    echo "$HITS" >&2
    FAIL=1
  fi
done
if [ "$FAIL" -eq 0 ]; then
  echo "  no host paths anywhere under prebuilt/"
  echo "  configuration string is host-neutral:"
  llvm-strings "$OUT/arm64-v8a/lib/libavcodec.so" | grep -F -- "--prefix=" | head -1 | cut -c1-160
fi

echo
echo "=== sizes ==="
find "$OUT" -name '*.so' -exec ls -l {} \; | awk '{printf "%9d  %s\n", $5, $NF}'
for ABI in arm64-v8a armeabi-v7a x86_64 x86; do
  printf "%-14s %s\n" "$ABI" "$(du -ch "$OUT/$ABI/lib"/*.so | tail -1 | cut -f1)"
done

[ "$FAIL" -eq 0 ] || { echo; echo "ERROR: verification failed (see above)" >&2; exit 1; }

# The completion marker, written LAST — after every ABI is built and after
# every check above has passed.
#
# CI caches prebuilt/, and actions/cache's post step runs on a FAILED job too.
# A run that died partway through (say at the third ABI) therefore saves a
# two-ABI tree under the key, and every later run restores it, decides the
# build is already done, and fails at CMake for the missing ABI — with no way
# out that does not involve evicting the cache by hand. Presence of any
# particular library is not evidence the build finished; presence of this file
# is, because nothing else creates it and `rm -rf "$OUT"` at the top of this
# script removes it before anything is rebuilt.
# Written with content rather than touched empty: the workflow gates its cache
# SAVE on hashFiles() of this path, which reports nothing for a match it cannot
# hash.
{
  echo "built $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "abis arm64-v8a armeabi-v7a x86_64 x86"
} > "$OUT/.complete"
echo
echo "prebuilt/ is complete (marker written)"
