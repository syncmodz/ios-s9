#!/usr/bin/env python3
"""Dump the HFS+ volume header of an image or a raw block device file.

Reading the volume header answers questions that matter for the launchd read
failure:
  * blockSize      -- the allocation block size; MapFileBlockC divides by it
  * totalBlocks    -- sanity check that the volume is what we think
  * journalInfoBlock + the journaled/unmounted/inconsistent attribute bits
                     -- a dirty journal means XNU replays it at mount, and a
                        bad replay rewrites extents: right size, wrong bytes
  * catalog/extents fork sizes -- proves where the B-trees start

  hfsinfo.py <file-or-device> [offset]
"""
import struct
import sys

SIG = {0x482B: "H+", 0x4858: "HX"}
ATTR = [
    (7, "hw-locked"), (8, "unmounted"), (9, "spared-blocks"),
    (10, "no-cache-required"), (11, "INCONSISTENT"), (12, "cnid-reused"),
    (13, "JOURNALED"),
]


def main():
    path = sys.argv[1]
    base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 1024
    with open(path, "rb") as f:
        f.seek(base)
        h = f.read(512)
    if len(h) < 512:
        sys.exit("short read")

    sig, ver = struct.unpack(">HH", h[0:4])
    attrs = struct.unpack(">I", h[4:8])[0]
    lastmnt = h[8:12]
    jib = struct.unpack(">I", h[12:16])[0]
    fcount, dcount = struct.unpack(">II", h[32:40])
    bsize, total, free = struct.unpack(">III", h[40:52])
    nextalloc, = struct.unpack(">I", h[52:56])
    nextid, wcount = struct.unpack(">II", h[64:72])

    print("signature      %s (%#x) version %d" % (SIG.get(sig, "?"), sig, ver))
    print("attributes     %#010x  %s" % (
        attrs, " ".join(n for b, n in ATTR if attrs & (1 << b)) or "(none)"))
    print("lastMounted    %r" % lastmnt)
    print("journalInfoBlk %d (%s)" % (jib, "no journal" if jib == 0 else "journal present"))
    print("blockSize      %d" % bsize)
    print("totalBlocks    %d  => volume %.2f GiB" % (total, total * bsize / (1 << 30)))
    print("freeBlocks     %d" % free)
    print("files/folders  %d / %d" % (fcount, dcount))
    print("nextAlloc      %d" % nextalloc)
    print("nextCatalogID  %d  writeCount %d" % (nextid, wcount))

    for name, off in (("allocationFile", 112), ("extentsFile", 192),
                      ("catalogFile", 272), ("attributesFile", 352)):
        logical, clump = struct.unpack(">II", h[off:off + 8])
        totalb, = struct.unpack(">I", h[off + 8:off + 12])
        ext = struct.unpack(">8I", h[off + 16:off + 48])
        first = ext[0]
        cnt = ext[1]
        print("%-15s logical %-10d blocks %-8d firstExtent start=%d count=%d"
              % (name, logical, totalb, first, cnt))

    if bsize and (bsize & (bsize - 1)) == 0:
        print("\nallocation block %d bytes = %d sectors of 512"
              % (bsize, bsize // 512))


main()
