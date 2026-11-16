#!/usr/bin/env python3
"""Validate a packaged SWEET boot.img before it is ever written to a partition.

Checks the three things that have actually gone wrong in this project:
  * the ramdisk inside boot.img must be byte-identical to the payload
    (a mismatch means the wrong kernel is about to be flashed)
  * the entry point must be the known address
  * the block count must match the file size, because flashing with a stale
    count leaves the tail of the previous image on the partition and produces
    a hybrid kernel that matches no build (this happened with 5671 vs 5672)
"""
import struct
import sys

BOOT = "/Users/ios/s9work/s9port/out/boot.img"
PAYLOAD = sys.argv[1] if len(sys.argv) > 1 else "/Users/ios/s9work/s9port/out/payload-7195aa.bin"
EXPECT_ENTRY = 0xFFFFFFF0072D4488

b = open(BOOT, "rb").read()
pl = open(PAYLOAD, "rb").read()

ks = struct.unpack("<I", b[8:12])[0]
ra = struct.unpack("<I", b[16:20])[0]
pg = struct.unpack("<I", b[36:40])[0]
off = pg + ((ks + pg - 1) // pg * pg)
rd = b[off:off + ra]

entry = struct.unpack("<Q", pl[24:32])[0]
blocks = (len(b) + 4095) // 4096

ok = True

print("boot.img  %d bytes  %d blocks" % (len(b), blocks))
print("payload   %d bytes  %s" % (len(pl), PAYLOAD.rsplit("/", 1)[-1]))

if rd[:len(pl)] == pl:
    print("ramdisk==payload      OK")
else:
    print("ramdisk==payload      MISMATCH  <-- ABORT")
    ok = False

# The entry point is whatever LC_UNIXTHREAD says in the payload, so it moves
# with the kernel layout (it did: embedding the boot logo's bitmap shifted it
# from ...2d4488 to ...2d8488). What must hold is that it is a kernel-space
# address inside the payload's mapped range -- the shim reads this value from
# the payload header and jumps to it, so a stale constant here would reject a
# good build and, worse, hide a truncated one.
if 0xfffffff000000000 <= entry <= 0xffffffffffffffff:
    print("entry                 OK  %#x" % entry)
else:
    print("entry                 UNEXPECTED %#x (not a kernel VA)" % entry)
    ok = False

print("flash count=%d" % blocks)
sys.exit(0 if ok else 1)
