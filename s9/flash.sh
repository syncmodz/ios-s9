#!/bin/bash
# Write out/boot.img to sda20 the only way this project permits: push, verify
# the pushed copy, dd with fsync, then read the partition back and compare.
# Any divergence aborts before a reboot can happen.
#
#   ./flash.sh            flash and report
#   ./flash.sh --reboot   flash, and only if MATCH, reboot
#
# The safety contract (owner present + explicit OK for each boot) is not
# enforced here: this script only guarantees the write itself is sound.
set -uo pipefail
cd /Users/ios/s9work/s9port
export ADB_LIBUSB=1 ADB_MDNS=0

REBOOT=0
[ "${1:-}" = "--reboot" ] && REBOOT=1

H=$(shasum -a 256 out/boot.img | awk '{print $1}')
N=$(( ($(stat -f%z out/boot.img) + 4095) / 4096 ))
echo "target sha=$H blocks=$N"

if ! adb devices | grep -qw recovery; then
	echo "ABORT: device is not in TWRP recovery"
	exit 1
fi

PREV=$(adb shell "dd if=/dev/block/sda20 bs=4096 count=$N 2>/dev/null | sha256sum" | awk '{print $1}')
echo "previous=$PREV"
echo "$PREV  previous_sda20_${N}" > "$HOME/s9work/rollback-prev.txt"

adb push out/boot.img /tmp/b.img 2>&1 | tail -1
T=$(adb shell "sha256sum /tmp/b.img" | awk '{print $1}')
if [ "$T" != "$H" ]; then
	echo "ABORT: pushed copy does not match ($T)"
	exit 1
fi

adb shell "dd if=/tmp/b.img of=/dev/block/sda20 bs=4096 count=$N conv=fsync 2>&1 | tail -1; sync"

R=$(adb shell "dd if=/dev/block/sda20 bs=4096 count=$N 2>/dev/null | sha256sum" | awk '{print $1}')
echo "readback=$R"
if [ "$R" != "$H" ]; then
	echo "MISMATCH -- ABORT, do not reboot"
	exit 1
fi

echo "MATCH"
adb shell "rm -f /tmp/b.img; sync" >/dev/null 2>&1

cp out/boot.img "$HOME/s9work/boot-$(date +%Y%m%d-%H%M%S)-$N.img"
echo "archived to ~/s9work/"

if [ "$REBOOT" = 1 ]; then
	adb reboot
	echo "reboot issued $(date)"
fi
