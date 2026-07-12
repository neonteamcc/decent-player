# 14 — Full-Speed & UAC1 Support (in progress)

The driver was built against one point in the USB audio device space:
**high-speed bus × UAC2 × asynchronous feedback**. Full-speed devices
(12 Mbps — every DAC whose USB is served by a Bluetooth SoC, plus most
dongles and older async DACs) currently get 1/8th of the required data
rate and play continuous clicks. This document tracks the work to support
the rest of the space. The three axes are independent and each needs its
own handling:

| Axis | Works today | Being added |
|---|---|---|
| Bus speed | High-speed (125 µs microframes) | Full-speed (1 ms frames) |
| Audio class | UAC2 (clock entities, protocol 0x20) | UAC1 (endpoint rate control, protocol 0x00) |
| Sync type | Async + feedback EP | Adaptive / None (no feedback), Synchronous |

## Why full-speed breaks today (all four at once)

1. **Packet cadence**: `calibratedFpmf = rate / 8000.0` sizes ISO packets
   per 125 µs microframe. A full-speed device services one packet per
   **1 ms frame** → it receives 12.5% of the stream. Correct divisor at
   FS is 1000.
2. **Rate control**: UAC1 has no clock source entity; the UAC2 SET_CUR
   in `UsbAudioDevice.setSampleRate()` fails against every UAC1 device.
   UAC1 wants `bmRequestType 0x22, SET_CUR, wValue 0x0100, wIndex = data
   endpoint address, 3-byte rate` — sent *after* the streaming alt is
   active, re-sent after every alt change (see Linux `set_sample_rate_v1`).
3. **Descriptor layouts**: UAC1 Format Type I carries
   `bNrChannels@4, bSubframeSize@5, bBitResolution@6` and the discrete
   rate list *inside the descriptor*; the UAC2-offset parse reads garbage.
4. **Feedback format**: full-speed feedback is 3-byte Q10.14
   samples-per-frame (USB 2.0 §5.12.4.2), not 4-byte Q16.16 — and many
   FS devices have no feedback endpoint at all (adaptive/None sinks).

## Status

- [x] **Descriptor layer** (`com.decent.usbaudio.descriptor`): pure-Kotlin
  `UsbAudioDescriptorParser` producing a class-agnostic
  `UsbAudioDeviceLayout` — UAC version detection (bInterfaceProtocol +
  bcdADC), UAC1 Format Type I with rate lists, 9-byte audio endpoint
  descriptors (bRefresh/bSynchAddress), sync types, feedback endpoints,
  CS-endpoint bits (SamplingFrequency control, MaxPacketsOnly), UAC2
  clock source id, plus a descriptor-invariant "definitely high-speed"
  hint. JVM unit tests run against byte-exact reconstructions of captured
  dumps (`docs/hardware/fixtures/`), including the CSR/Qualcomm quirk of
  emitting EP_GENERAL *before* the standard endpoint descriptor.
- [x] Bus speed detection: `UsbAudioStream.nativeGetBusSpeed()`
  (USBDEVFS_GET_SPEED, kernel ≥ 4.13) with descriptor-invariant fallback
  and a class-based default (`UsbAudioDevice.detectBusSpeed`). bcdUSB is
  deliberately ignored — full-speed devices routinely report 0x0200.
- [x] UAC1 control requests: `UacControl` encodes endpoint
  SET_CUR/GET_CUR(SAMPLING_FREQ) (0x22/0xA2, 3-byte rates) and the UAC2
  clock-entity equivalents, wire-format unit-tested;
  `UsbAudioDevice.setSampleRate/readSampleRate/readClockValid` branch on
  the detected class. The wrapper's transition sequence is now
  class-specific: UAC1 activates the alt FIRST, then sets the rate on the
  endpoint (Linux `set_sample_rate_v1` ordering), with an echo readback
  and an advertised-rate check.
- [x] Native engine: runtime stream geometry — `packetsPerSecond`
  (1000 FS / 8000 HS), `packetsPerUrb` (2 FS / 8 HS), `numUrbs`
  (40 FS / 80 HS, same ~80 ms pipeline), passed from Kotlin through
  `nativeUsbAudioCreate`; feedback decode by payload length (3 B Q10.14 /
  4 B Q16.16) with ALSA-style freqshift auto-detect and re-arm; feedback
  URB length = the sync endpoint's wMaxPacketSize; no-feedback nominal
  pacing (adaptive/sync/None devices); per-packet wMaxPacketSize clamp
  (the kernel rejects oversize iso packets with EMSGSIZE).
- [x] Bit-depth reduction: TPDF-dithered `ditherInt24ToInt16` /
  `ditherInt32ToInt16` wired into both `nativeUsbAudioWriteRaw` and the
  native FLAC engine's conversion matrix (24-bit FLAC on a 16-bit device).
- [x] In-family integer decimation: `decimator.cpp` — 63-tap Kaiser
  (β 9.6) half-band FIR per ÷2 stage, cascaded for ÷4, double-precision
  filtering over canonical int32 with streaming state (phase carries
  across calls; reset on flush). Host-verified: passband −0.0005 dB,
  stopband leak −104 dB, ÷4 cascade clean. Wired into all three write
  paths (WriteRaw, float write, native FLAC engine); the wrapper maps
  unsupported rates via `chooseUsbRate` (÷2 → ÷4, full-speed bandwidth
  checked), runs the DAC at the mapped rate, and tracks position in the
  output domain. Passthrough (`decimationFactor = 1`) leaves every legacy
  path byte-identical.
- [x] **Validated on hardware: FiiO BTR13** (full-speed × UAC1 ×
  no-feedback, 16-bit) — real-device playback confirmed clean by the
  device owner (2026-07-11), CI-built AARs, release APK.
- [ ] Remaining validation: full-speed UAC2 hardware (Apple dongle-class —
  no device on hand; logic covered by unit tests on its descriptor dump),
  xHCI ftrace packet-size verification on a rooted device, long-run
  (30+ min) adaptive drift soak, high-speed UAC2 regression pass
  (KA17-class — expected byte-identical, worth one explicit listen).

Reference behavior throughout is Linux `snd-usb-audio`
(`sound/usb/endpoint.c`, `clock.c`, `format.c`): nominal-rate fractional
packetization for adaptive/sync sinks (44.1 kHz at FS = nine 44-frame
packets + one 45-frame packet per 10 ms), 4 permanently-in-flight
single-packet feedback URBs paced by bRefresh, per-packet sanity windows
`freqn − freqn/8 … freqn + freqn/2`.

Contributions of descriptor dumps for full-speed devices are very welcome
— see `docs/hardware/fixtures/README.md` for the format and
[CONTRIBUTING.md](../../CONTRIBUTING.md).

- [x] Cross-family rational resampling: `resampler.cpp` — polyphase
  Kaiser windowed-sinc (100 dB stopband, passband flat to 20 kHz for
  44.1↔48 pairs), streaming L/M with phase carry. Host-verified:
  passband +0.0000 dB, alias rejection −107…−119 dB, exact frame
  counts on odd chunks. Covers 44.1-family tracks on 48-only devices
  (Apple dongle class) and pins MaxPacketsOnly firmwares (BTR3 class)
  to the one rate where nominal packet == wMaxPacketSize.
- [x] UAC2 rate discovery: RANGE(SAM_FREQ) on the clock source at open
  (`UacControl.uac2GetSampleRateRange` + parser, unit-tested); the rate
  picker consults the device list instead of staying permissive.

## Rate policy (decided)

Tracks whose sample rate exceeds the device's ceiling still play:
in-family integer decimation to the highest supported rate
(192k → 96k ÷2, 384k → 96k ÷4, 176.4k → 88.2k ÷2) via a half-band FIR —
deterministic, cheap, no asynchronous SRC. Combined with dithered
bit-depth reduction when needed (192/24 → 96/16 on a 16-bit/96k device).
Cross-family fractional SRC (44.1-family source onto a 48-only device)
is out of scope until a real device needs it. The engagement badge
reports the honest mode; bit-perfect claims are made only when the
stream is untouched.
