# decent-audio-ffmpeg

An audio-only FFmpeg (plus libmp3lame) built for the four Android ABIs, used
by the app's **download pipeline** — converting already-downloaded tracks
between formats on device.

**It is not a playback path.** Playback stays on the USB audio driver and on
media3/ExoPlayer; nothing here is ever on the audio hot path. That is why the
build is optimised for size (`-Os`) rather than for throughput, and why the
codec set is a short whitelist instead of "whatever might turn up".

## What this module contains

Two scripts and this document. **No upstream source and no binaries are
committed** — same convention as `decent-media3-decoder-flac`, which clones
`xiph/flac` at build time. Both the fetched sources and the build output are
gitignored:

```
setup.sh          fetch FFmpeg + LAME sources into src/main/jni/upstream/
build-ffmpeg.sh   cross-compile both for arm64-v8a, armeabi-v7a, x86_64, x86
prebuilt/         build output (gitignored)
  <abi>/lib/lib{avcodec,avformat,avutil,swresample}.so
  <abi>/include/  per-ABI headers as installed
  include/        the same headers published once, for consumers' CMake
  COPYING.ffmpeg.LGPLv2.1, COPYING.lame.LGPLv2
```

Four shared libraries per ABI, and nothing else to package. LAME is staged
into the ffmpeg source tree (`upstream/ffmpeg/deps/<abi>/`) rather than into
the published prefix, so `libmp3lame.a` and `lame/*.h` never appear in
`prebuilt/` at all — it is a build-time input that has already been folded
into `libavcodec.so`, and nothing downstream should be able to link it a
second time. Beyond the four, the only `DT_NEEDED` entries are `libm`,
`libz`, `libc` — all Android platform libraries, nothing extra to ship.

`lib/pkgconfig/` and `share/` are deleted after each ABI. The `.pc` files
have no consumer (consumers link these libraries by explicit path, not
through pkg-config) and their `prefix=` line is an absolute build path;
`share/` is ffmpeg's example `.c` files plus LAME's man page, which
`--disable-doc` does not suppress and which would otherwise be published
four times over.

The licence texts travel with the binaries: shipping shared libraries is
how the LGPL's relinking requirement is satisfied here, and the notice is
the other half of that obligation.

Pinned upstream revisions live at the top of `setup.sh`: FFmpeg `n8.1.2` and
LAME `3.100`.

## Building locally

```bash
cd libs/decent-audio-ffmpeg
bash setup.sh
ANDROID_NDK_ROOT=~/Library/Android/sdk/ndk/29.0.14206865 bash build-ffmpeg.sh
```

In CI the NDK is at `$ANDROID_HOME/ndk/29.0.14206865`. Nothing else is
required — FFmpeg and LAME use their own `configure`, so no CMake is needed
for this module. Four ABIs take tens of minutes; that is normal.

The host toolchain tag (`linux-x86_64` in CI, `darwin-x86_64` on a
maintainer's Mac) and the job count (`nproc` vs `sysctl`) are both detected,
never hardcoded — hardcoding either makes the script runnable in exactly one
place.

## The configure line, and why it is what it is

Four decisions in `build-ffmpeg.sh` are load-bearing. Do not "simplify" them.

**No `--enable-gpl`.** libmp3lame is LGPL and does not require it; only
GPL-only encoders (x264 and friends) would, and we build none. Adding the
flag would change the license posture of a public artifact for nothing.
`configure` prints `License: LGPL version 2.1 or later` — that line is the
check.

**Shared, not static.** LGPL requires that a user be able to relink the app
against their own build of FFmpeg. Shipping `libavcodec.so` beside our
wrapper satisfies that by construction. Statically folding FFmpeg into our
own `.so` would not, and would put us in the position of having to ship
object files instead.

LAME itself is built *static* and folded into `libavcodec.so`. That is not a
contradiction: libavcodec is what remains replaceable, and it is the unit the
license cares about.

**Unversioned SONAMEs.** Android's packager only extracts files matching
`lib*.so`, so a default `libavcodec.so.62` never reaches a device — it is
silently absent at `dlopen` time. FFmpeg's own `--target-os=android` case
already handles this (`SLIB_INSTALL_NAME=$(SLIBNAME)`, empty
`SLIB_INSTALL_LINKS`, `-Wl,-soname,$(SLIBNAME)`), so no manual `SLIBNAME=`
overrides are needed — and `configure` would reject them anyway, since it
does not accept arbitrary `VAR=value` arguments. Verify after a build:

```bash
NDK=~/Library/Android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64
$NDK/bin/llvm-readelf -d prebuilt/arm64-v8a/lib/libavcodec.so | grep SONAME
```

Expected `libavcodec.so`, with no `.62` suffix.

**API 28 floor.** Matches the app's `minSdk`. A higher floor here would ship
a download pipeline that some supported devices cannot load at all.

Three smaller notes:

- `--extra-ldflags` carries `-Wl,-z,max-page-size=16384` for Android 15's
  16 KB page sizes. NDK r28+ links that way by default, but these `.so` files
  come out of upstream `configure` scripts rather than the NDK's own build
  systems, so nothing else here would guarantee it.
- `setup.sh` refreshes LAME's `config.sub`/`config.guess` from GNU config.
  The 2013 copies LAME 3.100 ships reject `aarch64-linux-android` outright,
  so its `configure` never starts. Those files only canonicalise host
  triples; they do not affect a byte of generated code.
- `--disable-x86asm` on the `x86` and `x86_64` ABIs only, plus
  `--disable-inline-asm` on 32-bit `x86`. FFmpeg's x86 SIMD is nasm-syntax
  assembly, so `configure` hard-fails on those two ABIs unless nasm is
  installed on the *host*; disabling it unconditionally is preferred to
  gating on "is nasm present", which would make the produced binaries depend
  on which machine built them. 32-bit x86 needs the second flag as well,
  because `libavcodec/x86/lpc_init.c` (the FLAC encoder's autocorrelation)
  emits `R_386_32` against a local symbol and cannot be linked into a shared
  object — the build dies at LD asking to "recompile with -fPIC" when -fPIC
  is already on. arm64-v8a and armeabi-v7a — the entire real device fleet —
  keep all of their assembly, and are the only ABIs where it matters.

## Verifying a build

`build-ffmpeg.sh` verifies itself and exits non-zero if anything below fails,
so a green run is the check. What it asserts, and why those particular
assertions:

- **Every SONAME on every ABI is unversioned.** This is the failure mode the
  whole module is arranged around, and it is invisible until a device fails
  to `dlopen`, so it is asserted 16 times rather than spot-checked once.
- **The codec set is really enabled**, asserted per ABI against the
  generated `config_components.h` immediately after each `configure` — all
  25 components plus `CONFIG_LIBMP3LAME` (which lives in `config.h`, not
  `config_components.h`), and a handful of negative controls that must be
  `0`. See below for why nothing else works.
- **libmp3lame really was folded in**, from the `LAME3.100` banner in
  `libavcodec.so`. Unlike a codec `long_name`, that string comes from the
  LAME sources, so it is real evidence.
- **No host path is present in any published file** — see "Host-neutral
  builds" below.

### Two checks that look right and prove nothing

Do not "simplify" the codec assertion back into either of these.

`nm -D | grep ff_flac_encoder` returns zero on a *correct* build: `ff_*`
codec structs are hidden symbols and ffmpeg's version script limits the
dynamic symbol table to the public `av*` API.

Grepping the codecs' `long_name` strings is worse, because it appears to
work. `libavcodec/codec_desc.c` compiles a descriptor for **every** codec
unconditionally, so those strings survive `--disable-everything` whether or
not the codec is built. Measured on exactly this configuration:

| grepped in `libavcodec.so` | hits | this build can actually… |
|---|---:|---|
| `FLAC (Free Lossless Audio Codec)` | 1 | yes |
| `H.264 / AVC` | 1 | **no** |
| `Apple Lossless Audio Codec` | 1 | **no** |
| `Opus` | 1 | **no** |
| `Vorbis` | 2 | **no** |

So the string proves a descriptor exists, not an encoder, and a build that
silently lost `--enable-encoder=flac` would print the same green result.
(`PCM signed 24-bit little-endian` additionally substring-matches its own
`…planar` variant.) `config_components.h` is ffmpeg's own authoritative
record of what `configure` enabled, it is free to check, and it fails
*before* `make` rather than after.

That difference was confirmed, not assumed: re-running an otherwise
identical `configure` with `--enable-encoder=flac` dropped produced
`#define CONFIG_FLAC_ENCODER 0` and the assertion exited 1 with
`MISSING CONFIG_FLAC_ENCODER`, while the `FLAC (Free Lossless Audio Codec)`
string count in the binary stayed at 1.

## Host-neutral builds

ffmpeg's `configure` stores its **entire argument list** in
`FFMPEG_CONFIGURATION` (`config.h`), which every library then carries at
runtime as `avcodec_configuration()`. An absolute `--prefix`, `--cc` or
`--sysroot` is therefore baked into all four `.so` files. That is
unacceptable twice over:

- these artifacts are published from a **public** repository, so an absolute
  path ships the build machine's home directory and with it a username;
- the same commit built on a Mac and on a CI runner would produce
  byte-different libraries purely because the paths differ.

So no configure argument may contain a host path:

- `$TOOLCHAIN/bin` goes on `PATH` and the tools are named bare
  (`--cc=aarch64-linux-android28-clang`, `--cross-prefix=llvm-`,
  `--nm=llvm-nm`). NDK clang locates its own sysroot relative to itself, so
  `--sysroot` is not needed at all.
- `--prefix` is a fixed fictional path (`/decent-audio-ffmpeg`) and
  `make install DESTDIR=…` relocates the tree into `prebuilt/<abi>/`.
- libmp3lame is staged *inside* the ffmpeg source tree so ffmpeg can find it
  through a relative `-Ideps/<abi>/include` / `-Ldeps/<abi>/lib`; `configure`
  and `make` both run with the ffmpeg source root as cwd, so it resolves.

The build ends with a leak scan that asserts this on the finished artifacts
and exits non-zero on any hit — the failure is otherwise invisible until
someone runs `strings` on a published library:

```bash
strings prebuilt/*/lib/*.so | grep -c "$HOME"   # must be 0
grep -rl "$HOME" prebuilt/                      # must be empty
```

## Codec set

Deliberately narrow — every entry is something the download pipeline actually
handles. Widening it costs binary size on every install.

| | |
|---|---|
| Demuxers | mov, flac, mp3, aac, wav |
| Muxers | flac, mp4, ipod, mp3, wav |
| Decoders | flac, aac, mp3, eac3, pcm_s16le, pcm_s24le, pcm_f32le |
| Encoders | flac, libmp3lame, pcm_s16le, pcm_s24le |
| Parsers | flac, aac, mpegaudio |
| Protocols | file |

`avdevice`, `swscale`, `avfilter` and all networking are disabled;
`swresample` stays, because format conversion needs it.

If `prebuilt/arm64-v8a/lib` ever comes out far above ~6 MB, a
`--disable-everything` arm was lost — re-read the configure line before
shipping it, because everything downstream inherits that size.
