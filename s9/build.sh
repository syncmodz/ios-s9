#!/bin/bash
# Build the S9 shim and wrap it the way Samsung's ABL expects the stock
# kernel: boot.img header v0, Image.gz with the stock DTBs appended.
#   ./build.sh [m1|m2|m3|m5|m6] stage to build (default m2)
#   PAYLOAD=out/payload.bin ./build.sh m4   XNU payload as the ramdisk
#   out/boot.img      -> tools/flash-twrp.sh (verified dd), also kept as out/boot-STAGE.img
#   out/boot.tar.md5  -> Odin, AP slot
# Based on ios-sweet shim/build.sh (MIT, dipa-studio).
set -euo pipefail
cd "$(dirname "$0")"
# Arch's cross gcc unpacked into toolchain/root (no root needed), unless told otherwise.
# Those are Linux binaries: on macOS, Homebrew's aarch64-elf-gcc (same GCC 16 /
# binutils 2.47) and AOSP mkbootimg.py in toolchain/mac (see NOTES.md).
if [ -z "${CROSS_COMPILE:-}" ] && [ "$(uname)" = Linux ] && [ -x toolchain/root/usr/bin/aarch64-linux-gnu-gcc ]; then
	CROSS_COMPILE=toolchain/root/usr/bin/aarch64-linux-gnu-
fi
if [ -z "${CROSS_COMPILE:-}" ] && [ "$(uname)" = Darwin ] && command -v aarch64-elf-gcc >/dev/null; then
	CROSS_COMPILE=aarch64-elf-
fi
MKBOOTIMG=${MKBOOTIMG:-mkbootimg}
if ! command -v mkbootimg >/dev/null && [ "$MKBOOTIMG" = mkbootimg ] && [ -f toolchain/mac/mkbootimg.py ]; then
	MKBOOTIMG="python3 -I toolchain/mac/mkbootimg.py"
fi
CROSS=${CROSS_COMPILE:-aarch64-linux-gnu-}
CC=${CROSS}gcc
OBJCOPY=${CROSS}objcopy
READELF=${CROSS}readelf
NM=${CROSS}nm
STOCK_BOOT=${STOCK_BOOT:-../ZTO-G9600ZHU9FVF2/boot.img}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra ${EXTRA_CFLAGS:-}"

STAGE=${1:-m2}
case $STAGE in
m1) main="m1.c screen.c" ;;
m2) main="m2.c con.c" ;;
m3) main="m3.c con.c plat.c ufs.c mmu.c" ;;
m5) main="m5.c con.c plat.c" ;;
m6) main="m6.c con.c plat.c mmu.c" ;;
m7) main="m7.c con.c plat.c" ;;
m8) main="m8.c con.c plat.c" ;;
m9) main="m9.c con.c plat.c" ;;
m10) main="m10.c con.c plat.c" ;;
m11) main="m11.c con.c plat.c" ;;
m11d) main="m11d.c con.c plat.c" ;;	# M11.1: V3 diagnostic, one GPU-SMMU read per worker core
m11e) main="m11e.c con.c plat.c" ;;	# M11.2: GPU-SMMU translate (S9 arm-smmu.c) + GMU boot + HFI INIT/FW_VER
m11f) main="m11f.c con.c plat.c" ;;	# M11.3: m11e baseline + read-only RPMh cmd-db discovery + GX power-state readout
m11g) main="m11g.c con.c plat.c" ;;	# M11.4: m11f + OPP-derived GX/MX vote indices + 3z teardown fix + default-off gx_rail_on
m12) main="m12.c con.c plat.c" ;;	# M12:   m11g base + CP bring-up (SQE/ZAP/CP_ME_INIT/NOP/MEM_WRITE); CP gated -DM12_TRY_CP=1
m4) main="m4.c con.c plat.c loader.c"
    [ -f "${PAYLOAD:-}" ] || { echo "m4 needs PAYLOAD=path/to/payload.bin (tools/mkpayload.py)" >&2; exit 2; } ;;
*) echo "usage: build.sh [m1|m2|m3|m4|m5|m6|m7|m8|m9|m10|m11|m11d|m11e|m11f|m11g|m12]" >&2; exit 2 ;;
esac

mkdir -p out/obj
rm -f out/obj/*.o
objs=()
for f in start.S $main fb.c mdss.c fdt.c ramlog.c lib.c; do
	o=out/obj/${f%.*}.o
	"$CC" $CFLAGS -c "shim/$f" -o "$o"
	objs+=("$o")
done
# ABL runs us wherever it loads us, with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$("$READELF" -r "${objs[@]:1}" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
"$CC" $CFLAGS -static -T shim/shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	"${objs[@]}" -o out/shim.elf
"$OBJCOPY" -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stack exist as zeroed bytes.
size=$((0x$("$NM" out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
gzip -9 -n -c out/Image > out/Image.gz

# Same DTB list as the stock kernel: ABL still picks one by msm-id/board-id.
[ -f dtb/stock-dtbs.bin ] || python3 -I tools/extract-stock-dtbs.py "$STOCK_BOOT" dtb/stock-dtbs.bin
cat out/Image.gz dtb/stock-dtbs.bin > out/Image.gz-dtb

# Ramdisk: the XNU payload for m4; otherwise an empty newc cpio (some ABL builds
# dislike a zero-size ramdisk), fixed mtime and inodes so hashes are reproducible.
# Written here byte for byte as GNU `cpio -o -H newc --reproducible` made it on
# the Linux box (".", uid/gid 1002), which BSD cpio cannot do.
if [ "$STAGE" = m4 ]; then
	cp "$PAYLOAD" out/ramdisk.cpio.gz
else
	python3 -I -c '
import sys
def ent(name, mode, nlink, owner):
    n = name.encode() + b"\0"
    b = b"070701" + b"".join(b"%08X" % v for v in (0, mode, owner, owner, nlink, 0, 0, 0, 0, 0, 0, len(n), 0)) + n
    return b + b"\0" * (-len(b) % 4)
a = ent(".", 0o40755, 2, 1002) + ent("TRAILER!!!", 0, 1, 0)
sys.stdout.buffer.write(a + b"\0" * (-len(a) % 512))' | gzip -9 -n > out/ramdisk.cpio.gz
fi

# Header values from the stock G9600ZHU9FVF2 boot.img (unpack_bootimg).
$MKBOOTIMG --kernel out/Image.gz-dtb --ramdisk out/ramdisk.cpio.gz \
	--base 0x00000000 --kernel_offset 0x00008000 --ramdisk_offset 0x02000000 \
	--second_offset 0x00f00000 --tags_offset 0x01e00000 --pagesize 4096 \
	--board RILQI20A009 --os_version 10.0.0 --os_patch_level 2022-05 \
	--cmdline "console=null" -o out/boot.img

# Odin: tar with the image, followed by the tar's own md5 line.
(cd out && tar --format=ustar -cf boot.tar boot.img && md5sum boot.tar >> boot.tar && mv boot.tar boot.tar.md5)

# Host render of the M1 screen layout (fake probe values) for a quick look.
if [ "$STAGE" = m1 ]; then
	gcc -O1 -Wall -o out/host_test shim/host_test.c shim/fb.c shim/screen.c
	./out/host_test out/m1-preview.ppm && python3 -I tools/ppm2png.py out/m1-preview.ppm out/m1-preview.png 2
	rm -f out/m1-preview.ppm
fi
cp out/boot.img "out/boot-$STAGE.img"

ls -l out/Image out/boot.img out/boot.tar.md5
sha256sum out/boot.img
