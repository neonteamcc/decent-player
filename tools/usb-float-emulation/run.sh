#!/bin/sh
set -eu
# Usage: ./run.sh QEMU KERNEL INITRAMFS OUTPUT_DIRECTORY
qemu=$1
kernel=$2
initramfs=$3
mkdir -p "$4"
out=$(cd "$4" && pwd)
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 - "$out/guest.log" "$qemu" -machine virt -cpu cortex-a72 -accel tcg -m 384 -smp 1 \
    -nographic -monitor none -nic none -no-reboot \
    -kernel "$kernel" -initrd "$initramfs" \
    -append 'console=ttyAMA0 rdinit=/init panic=-1 loglevel=4' \
    -device qemu-xhci,id=xhci \
    -device "usb-audio-float-test,bus=xhci.0,capture=$out/capture.bin" \
    <<'PYTHON'
import subprocess,sys
with open(sys.argv[1], 'wb') as log:
    subprocess.run(sys.argv[2:],stdout=log,stderr=subprocess.STDOUT,timeout=120,check=True)
PYTHON
python3 "$here/check_capture.py" "$out/capture.bin" "$out/guest.log"
