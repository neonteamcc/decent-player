#!/bin/sh
set -eu
# Usage: ANDROID_NDK_HOME=... ./build-harness.sh DRIVER_REPO OUTPUT_BINARY
repo=$(cd "$1" && pwd)
out=$2
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to an installed Android NDK}"
case $(uname -s) in Darwin) host=darwin-x86_64;; Linux) host=linux-x86_64;; *) exit 2;; esac
cxx="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$host/bin/aarch64-linux-android28-clang++"
src="$repo/libs/decent-usb-audio-driver/src/main/jni"
"$cxx" -std=c++17 -O2 -static -pthread -I"$here/include" -I"$src" \
    "$here/harness.cpp" "$src/decimator.cpp" "$src/resampler.cpp" "$src/usb-float-processing.cpp" -o "$out"
