#!/bin/bash
# Package the SWEET boot image. Kept as a file (not an inline command) because
# the long one-liner is awkward to review line by line.
set -euo pipefail
cd /Users/ios/s9work/s9port
PAY=${1:-out/payload-7195aa.bin}
cp "$PAY" out/ramdisk.cpio.gz
PATH="$HOME/s9work/bin:$PATH" mkbootimg \
  --kernel out/Image.gz-dtb \
  --ramdisk out/ramdisk.cpio.gz \
  --base 0x00000000 --kernel_offset 0x00008000 \
  --ramdisk_offset 0x02000000 --second_offset 0x00f00000 \
  --tags_offset 0x01e00000 --pagesize 4096 \
  --board RILQI20A009 --os_version 10.0.0 --os_patch_level 2022-05 \
  --cmdline "console=null" -o out/boot.img
shasum -a 256 out/boot.img
stat -f%z out/boot.img
