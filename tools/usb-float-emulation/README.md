# USB IEEE_FLOAT software integration stand

This synthetic UAC2 high-speed device runs inside QEMU 10.2.0. The guest uses a
real Linux kernel, xHCI driver and usbfs ioctls. The harness includes the production
`usb-audio-output.cpp`; only Android logging is replaced. No ioctl is mocked.
The guest binary is statically linked with the Android NDK, including its real
Linux `usbdevice_fs.h` and JNI headers.

The device exposes stereo 32-bit signed PCM on alternate setting 1 and stereo
IEEE_FLOAT32 on alternate setting 2. Clock entity 0x10 accepts 44100, 48000 and
96000 Hz. OUT endpoint 0x01 has bInterval=1 and wMaxPacketSize=104; feedback IN
0x81 emits Q16.16 frames/microframe. It records each received packet with its rate
and alternate setting. All descriptors are authored synthetic fixtures, never
hardware captures. VID/PID 46f4:f032 is local test identification only.

## Build and run (macOS arm64 or Linux x86_64)

Requirements: C compiler, Python 3, ninja, pkg-config, glib development files,
Android NDK. Obtain the official QEMU 10.2.0 source and Alpine v3.23 aarch64
`netboot/vmlinuz-virt` and `netboot/initramfs-virt`. No guest root filesystem or
network access is required. Keep downloads and build outputs outside the repo.
`inputs.sha256` records the exact tested source/kernel/initramfs versions. Verify
those hashes before using downloaded files; Alpine's stable release URLs can
advance. Download locations:

- https://download.qemu.org/qemu-10.2.0.tar.xz
- https://dl-cdn.alpinelinux.org/alpine/v3.23/releases/aarch64/netboot/vmlinuz-virt
- https://dl-cdn.alpinelinux.org/alpine/v3.23/releases/aarch64/netboot/initramfs-virt

`synthetic-uac2-config.hex` is the descriptor returned by the emulated device in
the Linux guest. It is a software fixture, not physical-device evidence.

```sh
cp dev-audio-float.c "$QEMU_SOURCE/hw/usb/"
printf '%s\n' "system_ss.add(when: 'CONFIG_USB', if_true: files('dev-audio-float.c'))" >> "$QEMU_SOURCE/hw/usb/meson.build"
mkdir -p "$QEMU_BUILD"
cd "$QEMU_BUILD"
AR=/usr/bin/ar RANLIB=/usr/bin/ranlib "$QEMU_SOURCE/configure" --target-list=aarch64-softmmu --disable-docs \
  --disable-gtk --disable-sdl --disable-cocoa --audio-drv-list= \
  --disable-slirp --disable-gnutls --disable-capstone
ninja -j6 qemu-system-aarch64
cd "$STAND"
ANDROID_NDK_HOME="$NDK" ./build-harness.sh "$DRIVER_REPO" "$WORK/harness"
python3 make-initramfs.py "$WORK/initramfs-virt" "$WORK/harness" "$WORK/test-initramfs.gz"
./run.sh "$QEMU_BUILD/qemu-system-aarch64" "$WORK/vmlinuz-virt" \
  "$WORK/test-initramfs.gz" "$WORK/result"
```

The runner succeeds only after the guest reports all six combinations passed
and the independent byte oracle accepts the captured packets. It checks known
float bit patterns (including 1.25), a varying sequence to detect dropped or
reordered frames, PCM integer conversion, tiny and irregular
writes, continuous feedback calibration, packet geometry/rate and EOS residual
preservation with only zero padding. The harness prints the enumerated raw
configuration descriptor to the guest log.

Capture format: repeated little-endian uint32 `sample_rate`, `alternate_setting`,
`payload_bytes`, immediately followed by exactly `payload_bytes` raw OUT bytes.
No credentials, network addresses or personal paths are stored in captures.

## Proof boundary

This covers native packing and packet submission through a real Linux usbfs and
xHCI software stack. It does not test Android permission granting, Media3/JNI
array entry calls, the Kotlin alternate-setting chooser, physical USB controllers,
DAC firmware clock drift, electrical behavior or analog output. Feedback is an
ideal constant-rate synthetic source. Timing assertions inspect logical packet
geometry, not physical bus timing. The harness selects the two advertised
alternates explicitly; descriptor/selection tests belong to the Kotlin suite.

Impact map: production float submission and EOS -> Linux usbfs URBs -> guest xHCI
-> synthetic UAC2 endpoint -> independent host byte oracle. No iOS/web changes,
persistence changes, host audio output or global service installation.
