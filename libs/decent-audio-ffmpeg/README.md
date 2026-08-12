# decent-audio-ffmpeg

An audio-only FFmpeg (plus libmp3lame) built for the four Android ABIs, used
by the app's **download pipeline** — converting already-downloaded tracks
between formats on device.

**It is not a playback path.** Playback stays on the USB audio driver and on
media3/ExoPlayer; nothing here is ever on the audio hot path. That is why the
build is optimised for size (`-Os`) rather than for throughput, and why the
codec set is a short whitelist instead of "whatever might turn up".

## What this module contains

Two build scripts, a thin JNI wrapper, and this document. **No upstream source
and no binaries are committed** — same convention as
`decent-media3-decoder-flac`, which clones `xiph/flac` at build time. Both the
fetched sources and the build output are gitignored:

```
setup.sh          fetch FFmpeg + LAME sources into src/main/jni/upstream/
build-ffmpeg.sh   cross-compile both for arm64-v8a, armeabi-v7a, x86_64, x86
arm32-smoke.sh    run armeabi-v7a under qemu-arm (see "The arm32 smoke test")
build.gradle.kts  the AAR: compiles the wrapper, packages the prebuilt libs
consumer-rules.pro  R8 keeps that travel inside the AAR (see "R8 / ProGuard")
src/main/jni/     CMakeLists.txt + flowy_audio_jni.cc  (the wrapper)
src/main/java/    com.decent.audio.FlowyFfmpeg          (the public surface)
src/androidTest/  the on-device proof (see "Device proof")
  assets/         two committed synthetic fixtures this build cannot synthesise
src/test/cpp/     arm32_smoke.c — the emulated proof, CI only
prebuilt/         build output of build-ffmpeg.sh (gitignored)
  .complete       written last; CI's "the build really finished" marker
  <abi>/lib/lib{avcodec,avformat,avutil,swresample}.so
  <abi>/include/  per-ABI headers as installed
  include/        the same headers published once, for consumers' CMake
  COPYING.ffmpeg.LGPLv2.1, COPYING.lame.LGPLv2
prebuilt-static/  armeabi-v7a/*.a for the smoke test only (gitignored)
prebuilt-jnilibs/ prebuilt/<abi>/lib/*.so restaged as <abi>/*.so (gitignored)
prebuilt-assets/  the COPYING files restaged as assets/ (gitignored)
```

The last two exist because the AAR packages jniLibs as `<abi>/*.so` — the
`lib/` level in build-ffmpeg.sh's install prefix has to come out — and because
the LGPL notices have to ride *inside* the artifact rather than merely exist in
a gitignored build tree. Both are regenerated from `prebuilt/` on every build
by the `stageJniLibs` / `stageLicenses` tasks.

## The published artifact

`cc.neonteam.decent:decent-audio-ffmpeg`, ~5.5 MB, containing:

| | |
|---|---|
| `jni/<abi>/libflowyaudio.so` | the wrapper, one per ABI |
| `jni/<abi>/lib{avcodec,avformat,avutil,swresample}.so` | FFmpeg, one set per ABI |
| `assets/licenses/COPYING.ffmpeg.LGPLv2.1`, `COPYING.lame.LGPLv2` | the notices |
| `classes.jar` | `com.decent.audio.FlowyFfmpeg` |

Twenty `.so` entries in total. Per ABI, `libflowyaudio.so` plus its four
FFmpeg libraries:

| ABI | wrapper | total |
|---|---:|---:|
| arm64-v8a | 334 KB | 2.6 MB |
| armeabi-v7a | 214 KB | 2.4 MB |
| x86_64 | 327 KB | 2.8 MB |
| x86 | 302 KB | 3.1 MB |

An app that splits by ABI ships one column, not the sum.

`libflowyaudio.so`'s only `DT_NEEDED` entries beyond the four are `liblog`,
`libm`, `libdl`, `libc` — all Android platform libraries. Every `LOAD` segment
in all twenty files is aligned to `0x4000`: the wrapper's `CMakeLists.txt`
passes `-Wl,-z,max-page-size=16384` for the same reason `build-ffmpeg.sh` does,
and it is not optional here — a wrapper without it fails to load beside
libraries that have it on an Android 15+ device with 16 KB pages.

## The JNI surface, and why it is not a command line

The obvious shape for an FFmpeg binding is `int run(String[] argv)` delegating
to `fftools/ffmpeg.c`. **That cannot be built against these libraries**, and
the reason is structural rather than incidental: ffmpeg's CLI is written on top
of libavfilter — it constructs a filter graph for every job, stream copy
included — and the configure line above carries `--disable-avfilter` (and
`--disable-programs`, so `fftools` is not compiled at all). The generated
`config.h` says so outright: `CONFIG_AVFILTER 0`, `CONFIG_FFMPEG 0`.

Getting a command line back would mean re-enabling avfilter plus the audio
filter set, *and* carrying `fftools`' dozen-plus source files across every
FFmpeg upgrade. That is precisely what ffmpeg-kit did, and ffmpeg-kit was
archived in 2025.

So the surface is typed. The whole operation set the download pipeline needs is
four things, and none of them is a filter graph:

| operation | route |
|---|---|
| stream copy (remux, passthrough) | demux → `avcodec_parameters_copy` → mux |
| MP3 320 from AAC | decode → swresample → libmp3lame |
| FLAC at a pinned 24 bits from E-AC-3 | decode → swresample → flac |
| stream copy + metadata + cover into MP4 | the above, plus an `attached_pic` stream |

Sample format, sample rate and channel layout conversion between a decoder's
output and an encoder's input is exactly what libswresample is for, and
swresample *is* enabled. It also rematrixes, which is why a multichannel source
reaching the MP3 branch is downmixed rather than refused — no libavfilter
needed for that either.

```java
package com.decent.audio;

public final class FlowyFfmpeg {
    public static final int OK = 0;

    public static final class AudioInfo {
        public final String codecName;      // ffmpeg's own name: "flac", "eac3", …
        public final int channels;
        public final int sampleRate;
        public final int bitsPerRawSample;  // 0 when the codec does not say
        public final long durationMs;       // 0 when the container does not say
        public final String formatName;     // the demuxer's name
        public final long bitRate;          // 0 when unknown
    }

    public static native AudioInfo probe(String path);          // null on failure
    public static        String    probeCodec(String path);     // convenience over probe
    public static        int       probeChannels(String path);
    public static        int       probeSampleRate(String path);

    // metadata is a flat [key, value, …] array, or null. The output carries
    // those tags and NOTHING else — see "Metadata" below.
    // coverPath is a JPEG or PNG file, or null — stored verbatim, never decoded,
    // which is why no image codec has to be built in.
    public static int remux(String src, String dst, String muxer,
                            String[] metadata, String coverPath);
    public static int encodeMp3(String src, String dst, int bitrateKbps,
                                String[] metadata, String coverPath);
    public static int encodeFlac(String src, String dst, int bitsPerSample,
                                 String[] metadata, String coverPath);

    // Same three with the source's own tags kept or dropped explicitly.
    public static final boolean INHERIT_SOURCE_METADATA = true;
    public static final boolean STRIP_SOURCE_METADATA   = false;  // the default
    public static int remux(…, boolean inheritSourceMetadata);
    public static int encodeMp3(…, boolean inheritSourceMetadata);
    public static int encodeFlac(…, boolean inheritSourceMetadata);

    // Same three with cancellation and progress; both arguments may be null.
    public static int remux(…, Job job, ProgressListener progress);
    public static int encodeMp3(…, Job job, ProgressListener progress);
    public static int encodeFlac(…, Job job, ProgressListener progress);

    // And the full form of each.
    public static int remux(…, boolean inheritSourceMetadata,
                            Job job, ProgressListener progress);

    public static final int CANCELLED = -1414092869;   // ffmpeg's AVERROR_EXIT

    public static final class Job implements AutoCloseable {
        public void cancel();          // from any thread, any time
        public boolean isCancelled();
        public void close();           // safe while the operation still runs
    }

    public interface ProgressListener {
        void onProgress(long positionMs, long durationMs);
    }

    public static native boolean hasEncoder(String name);
    public static native boolean hasDecoder(String name);
    public static native String  version();
    public static native String  lastError();
}
```

**Cancellation** is a native atomic behind an opaque handle, not a Java field:
it is read from libavformat's `AVIOInterruptCB` — which fires inside a blocked
read or write, on the operation's own thread — and written by whichever thread
pressed cancel, so neither side can afford to need a `JNIEnv`. The flag is
polled in the demux/decode loop as well, because a lossless transcode is
CPU-bound and the interrupt callback alone would let it run to the end of the
track first. A cancelled operation returns `CANCELLED` and leaves no
destination file, exactly like a failed one.

**Progress** is reported on the operation's thread, against the *source's*
timeline, coalesced to one callback per 250 ms of material plus a final tick at
completion. A listener that throws aborts the operation rather than being
swallowed.

`muxer` is an ffmpeg muxer name (`flac`, `mp4`, `ipod`, `mp3`, `wav`) or `null`
to guess from the destination's extension. **`ipod` is not a synonym for `mp4`
with an `.m4a` name:** its tag table (`codec_ipod_tags` in `movenc.c`) lists
AAC, ALAC and AC-3 and *not* E-AC-3, so an Atmos stream copy must ask for
`mp4`. Asserted both ways in `FlowyFfmpegFixtureTest`. `bitsPerSample` is 16 or 24; 24 goes
through `AV_SAMPLE_FMT_S32` with `bits_per_raw_sample = 24`, which is the pair
`flacenc.c` reads as "24-bit" (`bps_code` 6) — it shifts the samples down
itself, so nothing upstream has to pre-scale them.

Every operation returns `OK` or a non-zero failure; `lastError()` is the
human-readable channel and combines our own message with the last error
libav* logged. **It is thread-local** — read it on the thread that ran the
operation. Paths are filesystem paths, not content URIs: `file` is the only
protocol in this build.

## Metadata: container provenance is stripped, always

An output carries the caller's `metadata` and nothing else. Two separate things
had to be switched off for that to be true, and both are unconditional in the
default mode:

1. **The source's tags are not copied** — neither the container dictionary nor
   the stream one. The pipeline's sources are MP4-wrapped audio from a
   streaming platform, and an MP4 carries `major_brand`, `minor_version`,
   `compatible_brands` and `encoder` at container level plus `handler_name` /
   `vendor_id` per stream. Copied into a FLAC they become Vorbis comments and
   stay in the user's library forever. They can also be *false*: a Dolby Atmos
   source downmixed to plain 5.1 still carries `compatible_brands=mp42dby1`,
   i.e. a text tag claiming Dolby about audio that is no longer Dolby. A tag
   that lies about the file is worse than a missing one.
2. **libavformat does not sign the output.** Left alone, `mux.c`'s `init_muxer`
   writes `encoder=LIBAVFORMAT_IDENT` into the output's metadata (`mux.c:352`,
   with flags `0`, so a caller cannot pre-empt it by setting the key), which
   becomes a Vorbis comment in FLAC and a `TSSE` frame in MP3, and `movenc`
   adds a `©too` atom of its own (`movenc.c:4764`). `AVFMT_FLAG_BITEXACT` on
   the output context is the only switch for all three; for the muxers this
   build has it does nothing else. It is deliberately *not* set on the
   encoders, so libmp3lame's DSP path is untouched.

Pass `INHERIT_SOURCE_METADATA` to get the old behaviour: the source's tags
first, the caller's on top. **Only in that mode does an empty value in
`metadata` mean "remove this key"** — with nothing inherited there is nothing
to remove. The stamp suppression is not part of the switch: even an inheriting
operation drops `encoder`, because libavformat deletes that key under bitexact
(`mux.c:355`).

Two things are out of reach and both are structure rather than tags: an MP4
output's `ftyp` brands are `movenc`'s own and describe the file it just wrote,
and an MP3's Xing header has a fixed 9-byte encoder field that reads `Lavf`
whatever we do (`mp3enc.c:257`) — readers key gapless delay/padding detection
off it. For the same reason a FLAC's Vorbis vendor field cannot be empty; under
bitexact it reads `ffmpeg` instead of the version (`flacenc.c:66`).

`FlowyFfmpegMetadataTest` proves all of this on a device, in both directions,
against a fixture that really does carry the four MP4 tags.

Two things this wrapper deliberately does *not* do, so that a later caller does
not discover them the hard way:

- **No bitstream filters are applied.** None of the four operations needs one.
  A case that would — ADTS `.aac` stream-copied into MP4, which needs
  `aac_adtstoasc` — is not in the pipeline; it would fail at
  `avformat_write_header` with a message saying so, not silently.
- **The source's embedded artwork is not carried across.** A stream copy takes
  the audio stream only; `coverPath` is the sole way art gets into the output.
  That is deliberate for this caller — the pipeline's tag writer supplies the
  cover from the track's own cover URL, and keeping both would write the
  artwork twice.

## R8 / ProGuard

The AAR ships its own keep rules (`consumer-rules.pro` →
`consumerProguardFiles` → `proguard.txt` inside the artifact), so a consuming
app inherits them and cannot forget them.

They are not optional. `proguard-android-optimize.txt`'s
`-keepclasseswithmembernames class * { native <methods>; }` saves `FlowyFfmpeg`
and the names of its native methods — enough for JNI's automatic binding — and
saves nothing that the `.so` looks up by name. `flowy_audio_jni.cc` does
exactly that in two places: `FindClass` + `GetMethodID("<init>")` + `NewObject`
on `FlowyFfmpeg$AudioInfo` (whose constructor has no Java caller at all, so it
is stripped outright without a keep) and `GetMethodID("onProgress")` on the
`ProgressListener` implementation. Missing either is a release-only crash on
the first call and invisible in debug.

Confirm after changing anything here:

```bash
unzip -p decent-audio-ffmpeg-release.aar proguard.txt
```

## Device proof

Nothing about the codec set can be established by grepping the built binary
(see "Two checks that look right and prove nothing"). The check that cannot be
faked is `avcodec_find_encoder_by_name()` returning non-null on a device, so
that is what `src/androidTest/` asserts, alongside a full round trip: a WAV
tone synthesised on device is encoded to 24-bit FLAC, to 16-bit FLAC and to
MP3 320, stream-copied FLAC → FLAC, and stream-copied into MP4 with metadata
and a cover picture — each result read back with `probe`. Negative controls
assert that `aac`'s *encoder*, `opus` and `alac` are absent, which is what
proves `--disable-everything` still holds.

Two inputs cannot be synthesised by a build with this codec set, and they
happen to be the two the whole route change was made for — FLAC inside MP4 and
E-AC-3 5.1. Both ride as committed synthetic fixtures under
`src/androidTest/assets/` (a generated tone, regeneration commands in the
README beside them), and `FlowyFfmpegFixtureTest` proves on device what was
previously only read out of FFmpeg's sources: the MP4 `dfLa` extradata produces
a FLAC that still decodes, and a 5.1 E-AC-3 source reaches the FLAC encoder as
six channels at 24 bits. Cancellation and progress are asserted there too,
including a cancel issued from another thread while the encode is provably
still running.

`FlowyFfmpegMetadataTest` pins the metadata invariant on the same fixture: each
operation in the default mode, asserting the caller's tags are there and none
of the source's four MP4 tags are, then the same operations with
`INHERIT_SOURCE_METADATA` asserting they *are* — so a parameter that had
stopped doing anything would fail rather than pass quietly. Its assertions were
checked by planting the failure they exist to catch: with the
`AVFMT_FLAG_BITEXACT` line removed, five of its eight tests fail.

```bash
cd libs && ANDROID_SERIAL=<device> ./gradlew :decent-audio-ffmpeg:connectedDebugAndroidTest
```

Set `ANDROID_SERIAL`, or the run installs on every attached device.

It is not in CI: the FFmpeg build already dominates the job, and the x86_64
emulator a GitHub runner can host is not the ABI the fleet uses. Run it against
an arm64 emulator or a real device when the wrapper or the codec set changes.

## The arm32 smoke test

`arm64-v8a` is proven — an emulator run with 16 KB pages, then two real phones
in the field. **`armeabi-v7a` was built four times over and never once run.**
The maintainer owns no 32-bit device, and Apple silicon cannot execute AArch32
at all, so a local emulator would be full-system emulation rather than a
practical route. CI runs it instead, on every push, in under a second:

```bash
ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/29.0.14206865 bash arm32-smoke.sh
```

`src/test/cpp/arm32_smoke.c` is linked against the armeabi-v7a libraries
`build-ffmpeg.sh` just produced and run under `qemu-arm`. It decodes both
committed fixtures, encodes what it decoded back out through the FLAC encoder
and libmp3lame, and looks the whole codec set up by name at run time.

Three of its checks carry most of the weight.

The **decoded PCM is pinned by CRC**. FLAC decoding is exact integer
arithmetic, so the value is a property of the fixture rather than of our build
— the same `b934fd38` comes out of this build under emulation and out of a host
FFmpeg on a different architecture.

The **FLAC round trip is bit-exact**: what the FLAC encoder wrote has to decode
back to the same CRC. Note that this does *not* subsume the pinned value, and
that was established rather than assumed — flipping one byte of the fixture
changes the decoded CRC while leaving the round trip perfectly green, because
encoding wrong samples and decoding them back returns the same wrong samples. A
round trip proves the encoder and decoder agree with each other; only a pinned
value proves they agree with the format.

The **negative controls** (`h264`, `alac`, `opus`, `vorbis`) must come back
NULL, which is what proves `--disable-everything` held on this ABI too; it is
the same list `build-ffmpeg.sh` asserts against `config_components.h`, checked
here against what the library actually offers.

**What it proves:** the build is sound for that ABI — the code executes, the
ARM assembly is intact, the link is complete, the codecs are there. That is
where this actually breaks.

**What it does not prove:** anything about Android's dynamic loader — page
alignment, SONAMEs, `DT_NEEDED` resolution inside the app's lib dir — or the
JNI wiring. The harness is linked *statically*, which is not a shortcut but the
only option: qemu-user has no bionic dynamic linker to hand a shared-linked
Android binary. Those properties are asserted for all four ABIs by
`build-ffmpeg.sh`, and the JNI is one source file compiled per ABI, so the gap
is narrow. Closing it means running `src/androidTest/` on a physical arm32
device (Firebase Test Lab) — a separate decision.

The archives it links are not a second build: one `make` compiles each object
once (PIC, because `--enable-shared` demands it) and both forms come from those
same objects, which is why `--enable-static` is set for this ABI and no other.
They live in `prebuilt-static/` rather than `prebuilt/` because everything
under `prebuilt/` is either published or scanned as if it were, and an archive
is neither — ffmpeg's install rule strips shared libraries but not archives, so
their DWARF would carry the checkout's absolute path straight into the leak
scan. They are stripped of debug info on the way out for the same reason.

One host-dependent thing to know if you run it outside CI: a 32-bit Android
binary sets `personality(PER_LINUX32)` before `main()`, and an **arm64 kernel
refuses that** unless the CPU has 32-bit EL0 — Apple silicon has none. So it
aborts on an arm64 workstation before a single line of FFmpeg runs, and the
script says so rather than letting it read as a build failure. x86_64 kernels
accept it, which is what `setarch --32bit` has always relied on.

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

Then build the AAR, which compiles the wrapper and packages the result:

```bash
cd libs && ./gradlew :decent-audio-ffmpeg:assembleRelease
```

In CI the NDK is at `$ANDROID_HOME/ndk/29.0.14206865`. FFmpeg and LAME use
their own `configure`, so the two scripts above need no CMake; the wrapper does,
and it comes from the SDK (`sdkmanager "cmake;3.22.1"`) rather than from
`$PATH`. All four ABIs of FFmpeg take **1m45s on a GitHub runner** — the codec
set is small enough that each `make` is about thirteen seconds — and rather
longer on a laptop. CI caches `prebuilt/` anyway, on a key that hashes both
scripts and the NDK revision that compiled them. The wrapper itself builds in
seconds.

The cache is written by an explicit `actions/cache/save` step guarded by
`if: success()`, not by the combined action, and the "already built, skip it"
test is `prebuilt/.complete` — a marker `build-ffmpeg.sh` writes only after the
last ABI and the leak scan. The combined action's post step runs on a failed
job too, so a run that died at the third ABI used to save a two-ABI tree under
the key; every later run then restored it, skipped the build and failed at
CMake for the ABI that was never built, until someone evicted the cache by
hand. The presence of any one library is not evidence that a build finished.

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

`armeabi-v7a` also produces `.a` archives, and that is not a contradiction
either: nothing links them into anything shipped. They exist so the emulated
smoke test can produce a binary qemu-user can start, they land outside
`prebuilt/`, and no packaging step can see them. See "The arm32 smoke test".

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
- `setup.sh` refreshes LAME's `config.sub`/`config.guess` when the copies
  LAME 3.100 ships cannot do the job. The 2013 `config.sub` rejects
  `aarch64-linux-android` outright, so its `configure` never starts, and the
  2013 `config.guess` cannot identify an Apple-silicon build host. Those files
  only canonicalise triples; they do not affect a byte of generated code.
  Each candidate — the shipped one first, then GNU config, then the GCC
  mirror — is validated by *running* it on the triple that has to work, which
  is what a throttled response or a truncated download fails and a
  "starts with `#!`" check does not. Upstream cgit throttles consecutive
  `/plain/` requests and has served a CI runner a non-script; the mirror is
  there for that, and the validation is what makes trusting either safe.
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
- **The archives the arm32 smoke links exist**, asserted where the reason for
  them is in view rather than surfacing three CI steps later as a linker error
  about a missing `-lavcodec`.

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
grep -ralF -- "$HOME" prebuilt/                 # must be empty
```

`-a` is not optional in the second line. Without it the leg silently skips
every binary file, and it does so *inconsistently across hosts*: GNU `grep -l`
still lists a matching binary, but a ugrep installed as `grep` — common on
developer Macs — reports `Binary file matches` to stdout without listing it,
so a `-n`-style check reads as empty. Forcing text mode makes the result the
same everywhere. `-F` keeps a `$HOME` containing regex metacharacters literal.

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
