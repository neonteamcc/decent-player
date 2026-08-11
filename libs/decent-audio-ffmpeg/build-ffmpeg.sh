#!/bin/bash
# Builds an audio-only FFmpeg (+ libmp3lame) for the four Android ABIs.
#
# Run setup.sh first. Requires ANDROID_NDK_ROOT, e.g.
#   ANDROID_NDK_ROOT=~/Library/Android/sdk/ndk/29.0.14206865 bash build-ffmpeg.sh
#
# Output: prebuilt/<abi>/lib/lib{avcodec,avformat,avutil,swresample}.so
#         prebuilt/include/**   (headers, shared by all ABIs)
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

# Android 15+ devices may use 16 KB memory pages. NDK r28+ links with a
# 16 KB max-page-size by default, but we state it explicitly because these
# .so files are produced by upstream configure scripts, not by the NDK's
# own build systems — nothing else here would guarantee it.
PAGE_LDFLAGS="-Wl,-z,max-page-size=16384"

build_abi() {
  local ABI=$1 TRIPLE=$2 CPU=$3
  local PREFIX="$OUT/$ABI"
  local CC="$TOOLCHAIN/bin/${TRIPLE}${API}-clang"

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
  #
  # CFLAGS is repeated as a make-level override, and that is not redundant.
  # LAME's configure appends its own "OPTIMIZATION" flags derived from the
  # HOST cpu even in a cross build; for host_cpu=i686 that includes
  # -mtune=native, which the NDK clang resolves against the machine running
  # the build and rejects ("unknown target CPU 'apple-m4'"). Overriding
  # CFLAGS on the make command line drops the whole host-derived set, so all
  # four ABIs compile with exactly the flags we chose.
  ( cd "$SRC/lame" && { make distclean >/dev/null 2>&1 || true; }
    ./configure --host="$TRIPLE" --prefix="$PREFIX" \
      --disable-shared --enable-static --disable-frontend --disable-decoder \
      --disable-dependency-tracking \
      CC="$CC" AR="$TOOLCHAIN/bin/llvm-ar" RANLIB="$TOOLCHAIN/bin/llvm-ranlib" \
      NM="$TOOLCHAIN/bin/llvm-nm" STRIP="$TOOLCHAIN/bin/llvm-strip" \
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
  ( cd "$SRC/ffmpeg" && { make distclean >/dev/null 2>&1 || true; }
    ./configure \
      --prefix="$PREFIX" \
      --target-os=android --arch="$CPU" --enable-cross-compile \
      --cc="$CC" --cross-prefix="$TOOLCHAIN/bin/llvm-" --nm="$TOOLCHAIN/bin/llvm-nm" \
      --sysroot="$TOOLCHAIN/sysroot" \
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
      --extra-cflags="-Os -fPIC -I$PREFIX/include" \
      --extra-ldflags="-L$PREFIX/lib $PAGE_LDFLAGS" \
      --build-suffix="" \
      --enable-pic \
      $ASMFLAGS
    make -j"$JOBS" && make install )

  # LAME is a build-time input, not something we ship. Leave only ffmpeg's
  # shared libraries (and headers) in the published prefix.
  rm -f "$PREFIX/lib/libmp3lame.a" "$PREFIX/lib/libmp3lame.la"
  rm -rf "$PREFIX/include/lame"
}

rm -rf "$OUT"
build_abi arm64-v8a   aarch64-linux-android    aarch64
build_abi armeabi-v7a armv7a-linux-androideabi arm
build_abi x86_64      x86_64-linux-android     x86_64
build_abi x86         i686-linux-android       x86

# Task 2's CMake wants ONE include tree, not four. The installed headers are
# ABI-independent in practice (avconfig.h only records endianness/alignment
# traits that all four ABIs share), but assert it rather than assume it.
echo
echo "=== publishing shared headers ==="
for ABI in armeabi-v7a x86_64 x86; do
  if ! diff -r "$OUT/arm64-v8a/include" "$OUT/$ABI/include" >/tmp/ffmpeg-hdr-diff.$$ 2>&1; then
    echo "WARNING: headers differ between arm64-v8a and $ABI:"
    cat /tmp/ffmpeg-hdr-diff.$$
    echo "WARNING: prebuilt/include is the arm64-v8a tree — check before use."
  fi
  rm -f /tmp/ffmpeg-hdr-diff.$$
done
cp -R "$OUT/arm64-v8a/include" "$OUT/include"

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
    SONAME="$("$TOOLCHAIN/bin/llvm-readelf" -d "$SO" \
      | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p')"
    if [ "$SONAME" != "$LIB.so" ]; then
      echo "BAD SONAME  $ABI/$LIB.so -> '$SONAME'"; FAIL=1
    fi
  done
done
# Note the `if` rather than `&&`: under `set -e` a failing `&&` list at top
# level aborts the script, which would skip the checks and the summary below.
if [ "$FAIL" -eq 0 ]; then echo "SONAMEs OK (all unversioned, 4 ABIs x 4 libs)"; fi

# ff_* codec structs are hidden symbols — they never appear in `nm -D`, whose
# output is limited to the av* public API by ffmpeg's version script. The
# codecs' long_name strings do survive into the binary, and they come from
# the AVCodec structs themselves, so they are real evidence of what got
# compiled in.
echo "codec evidence in arm64-v8a/libavcodec.so:"
for NAME in "FLAC (Free Lossless Audio Codec)" "libmp3lame MP3" \
            "AAC (Advanced Audio Coding)" "PCM signed 24-bit little-endian" \
            "LAME3.100"; do
  N="$("$TOOLCHAIN/bin/llvm-strings" "$OUT/arm64-v8a/lib/libavcodec.so" \
        | grep -c "$NAME" || true)"
  printf "  %-34s %s\n" "$NAME" "$N"
  [ "$N" -gt 0 ] || FAIL=1
done

echo
echo "=== sizes ==="
find "$OUT" -name '*.so' -exec ls -l {} \; | awk '{printf "%9d  %s\n", $5, $NF}'
for ABI in arm64-v8a armeabi-v7a x86_64 x86; do
  printf "%-14s %s\n" "$ABI" "$(du -ch "$OUT/$ABI/lib"/*.so | tail -1 | cut -f1)"
done

[ "$FAIL" -eq 0 ] || { echo; echo "ERROR: verification failed (see above)" >&2; exit 1; }
