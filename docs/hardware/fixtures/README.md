# USB descriptor fixtures for the full-speed/UAC1 driver fork

Verbatim public descriptor dumps used as golden test vectors for the
descriptor parsers in the decent-usb-audio-driver fork (see
`../../driver/14-full-speed-uac1-support.md`). Nothing here
is reconstructed; each file states its source.

| File | Device | VID:PID | What it covers |
|---|---|---|---|
| `fiio-btr13-0a12-4007-thesycon.txt` | **FiiO BTR13 — the target device** (Thesycon dump from the device owner) | 0a12:4007 | UAC1, FS confirmed ("bus speed: FullSpeed"), single 16-bit alt, 4 discrete rates (44.1/48/88.2/96k), sync type **None**, no feedback EP, CS-EP sampling-freq only |
| `fiio-btr3-0a12-1243-lsusb.md` | FiiO BTR3 (same CSR/QCC family; includes `/proc/asound` stream dump) | 0a12:1243 | UAC1, 16-bit, 44.1/48k, sync None, **MaxPacketsOnly** bit set (fill_max branch), known 44.1k firmware bug |
| `fiio-btr3k-0a12-1004-lsusb.txt` | FiiO BTR3K | 0a12:1004 | same template as BTR3; 2-interface variant (no HID) |
| `dragonfly-black15-21b4-0083-lsusb.txt` | AudioQuest DragonFly Black v1.5 | 21b4:0083 | UAC1 **explicit async**: data EP bmAttributes 5 + `bSynchAddress 0x81`, 3-byte iso IN **feedback EP with bRefresh 5** (32 ms), 24-bit/`bSubframeSize 3`, 4 rates |
| `cmedia-cm108-0d8c-013c-lsusb.txt` | C-Media CM108 dongle | 0d8c:013c | UAC1 **adaptive** (bmAttributes 9), bcdUSB 1.10, multi-collection AC (mixer/selector units), mono capture alt |
| `realtek-alc4040-0bda-4040-lsusb.txt` | Realtek ALC4040 USB-C dongle | 0bda:4040 | UAC1 with **one-rate-per-alt-setting** (`bSamFreqType 1`), adaptive OUT; caveat: this probe enumerated at high speed (bInterval 4, Device Qualifier present) |
| `apple-usbc-35mm-05ac-110a-lsusb.txt` | Apple USB-C→3.5 mm adapter | 05ac:110a | **Negative fixture**: NOT UAC1 — 3 configurations: config 1 = UAC2 (bcdADC 2.00, protocol 0x20, clock source ID 9), configs 2/3 = UAC3/BADD (protocol 0x30). All data EPs **synchronous** (bmAttributes 13), no feedback. A UAC1 parser must reject it; the UAC2 parser must handle it at full speed |

Parser edge cases covered across the set: 9-byte vs 7-byte endpoint
descriptors; nonzero `bSynchAddress`/`bRefresh`; MaxPacketsOnly;
`bSamFreqType` 1/2/4 rate lists; UAC2-at-full-speed; multi-config devices;
SELECTOR/MIXER/EXTENSION units between IT and OT; `bInterfaceNumber 255`
oddity (ALC4040 HID).

**Implication worth remembering:** the Apple dump means USB-C EarPods (same
Apple audio family) are almost certainly **UAC2-at-full-speed, synchronous,
no feedback** — not UAC1. So our two physical devices cover complementary
combinations: BTR13 = FS × UAC1 × none; EarPods (to be confirmed with a
dump) = FS × UAC2 × synchronous. Both fail identically on the current
driver because the `rate/8000` microframe math starves any full-speed
device of 7/8 of its data, regardless of class.
