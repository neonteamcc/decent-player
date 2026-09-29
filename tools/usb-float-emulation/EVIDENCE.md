# Software integration evidence

Tested 2026-09-29 with QEMU 10.2.0, Alpine Linux 6.18.52-0-virt (aarch64),
Android NDK 29.0.14206865 static guest executable, and an immutable copy of the
production sources listed in `production-snapshot.sha256`.

Production source manifest SHA256:
`1491de6cf7103f416c1bc66a46a927c61fd765138864029fed568499715060a5`

Command: the `build-harness.sh` -> `make-initramfs.py` -> `run.sh` sequence in
README.md. Build, guest and capture checker exited 0. The final guest plus capture
checker invocation took 9.786 seconds wall time (measured with Python
`time.monotonic`; compilation and initramfs assembly excluded). Each stream used
chunks of 1/7/13/257/509 frames and 200 ms + 37 frames total.

| Format | Hz | Input frames | Exact source bytes | Zero padding | Packets | Frames per packet |
|---|---:|---:|---:|---:|---:|---|
| PCM32 | 44100 | 8857 | 70856 | 8 | 1607 | 784 x 5, 823 x 6 |
| PCM32 | 48000 | 9637 | 77096 | 40 | 1607 | 1607 x 6 |
| PCM32 | 96000 | 19237 | 153896 | 88 | 1604 | 1604 x 12 |
| IEEE_FLOAT32 | 44100 | 8857 | 70856 | 8 | 1607 | 784 x 5, 823 x 6 |
| IEEE_FLOAT32 | 48000 | 9637 | 77096 | 40 | 1607 | 1607 x 6 |
| IEEE_FLOAT32 | 96000 | 19237 | 153896 | 88 | 1604 | 1604 x 12 |

For every stream: high-speed enumeration; clock SET_CUR/GET_CUR and RANGE;
feedback calibration shift 0 and continuous feedback active; every expected
sample byte reached QEMU; `nativeFinish` left zero residual bytes and zero audio
URBs in flight. IEEE_FLOAT preserved +1.25, -1.25 and negative zero. The varying
remaining sample sequence checks order and content independently of the writer.

Capture SHA256 (719600 bytes):
`2f4794051863752063524eb2c8932214f19e32f1aa80c68014992c77803f5070`

The checker also rejected a copy of that capture with one flipped PCM payload
bit (`byte mismatch alt=1 rate=44100`). This is a negative control for the oracle.

The final source snapshot includes the completed JNI integration, valid-bit
handling, default-off limiter and PCM16 dither corrections. The guest invokes
the production `submitFloatPcm` helper directly. JVM array access and JNI write
dispatch remain outside this VM test's boundary; separate host JNI tests are
owned and reported by the lead. Do not use these source hashes as evidence for
subsequently changed files. See README.md for the other proof boundaries. No
device, analog-output or Android permission claims follow from this test.

## Final invocation

With `QEMU`, `KERNEL`, `BASE_INITRAMFS`, `DRIVER_SNAPSHOT`, `NDK`, `WORK` and
`STAND` pointing to the corresponding local inputs described in README.md:

```sh
ANDROID_NDK_HOME="$NDK" "$STAND/build-harness.sh" "$DRIVER_SNAPSHOT" "$WORK/harness"
python3 "$STAND/make-initramfs.py" "$BASE_INITRAMFS" "$WORK/harness" "$WORK/test-initramfs.gz"
"$STAND/run.sh" "$QEMU" "$KERNEL" "$WORK/test-initramfs.gz" "$WORK/result"
```

The VM runner already includes a 120-second timeout. The final capture hash and
six-case packet results above match the earlier helper-only snapshot exactly.
