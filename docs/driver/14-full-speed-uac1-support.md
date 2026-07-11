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
- [ ] Bus speed detection: `USBDEVFS_GET_SPEED` ioctl (kernel ≥ 4.13) with
  descriptor-hint fallback.
- [ ] UAC1 control requests (endpoint SET_CUR/GET_CUR sample rate) and the
  UAC1 alt→rate transition sequence in the wrapper.
- [ ] Native engine: runtime stream geometry (packets/sec 1000 vs 8000,
  packets/URB, ring depth, URB buffer size from wMaxPacketSize), feedback
  decode by payload length (3 B Q10.14 / 4 B Q16.16) with ALSA-style
  freqshift auto-detect, no-feedback nominal pacing, wMaxPacketSize clamp.
- [ ] Bit-depth reduction (dithered 24/32 → 16) for devices whose best
  format is shallower than the source.
- [ ] Validation: full-speed UAC1 (FiiO BTR13-class) and full-speed UAC2
  (Apple dongle-class) hardware, xHCI ftrace packet-size verification,
  high-speed UAC2 regression (existing devices must be byte-identical).

Reference behavior throughout is Linux `snd-usb-audio`
(`sound/usb/endpoint.c`, `clock.c`, `format.c`): nominal-rate fractional
packetization for adaptive/sync sinks (44.1 kHz at FS = nine 44-frame
packets + one 45-frame packet per 10 ms), 4 permanently-in-flight
single-packet feedback URBs paced by bRefresh, per-packet sanity windows
`freqn − freqn/8 … freqn + freqn/2`.

Contributions of descriptor dumps for full-speed devices are very welcome
— see `docs/hardware/fixtures/README.md` for the format and
[CONTRIBUTING.md](../../CONTRIBUTING.md).

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
