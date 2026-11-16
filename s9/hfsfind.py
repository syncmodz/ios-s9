#!/usr/bin/env python3
"""Find a file's catalog record inside an HFS+ image without walking the B-tree.

The catalog B-tree is keyed by (parentID, name); the name is stored as UTF-16BE.
So we can locate an entry by searching for the UTF-16BE name bytes and then
reading the key header that precedes it and the catalog record that follows.

Prints the on-disk truth for the fields that matter to the launchd exec bug:
logicalSize (what va_data_size must be), totalBlocks, and the first extents.

  hfsfind.py <image> <name> [startOffset] [limit]
"""
import struct
import sys

BE16 = lambda b, o: struct.unpack(">H", b[o:o + 2])[0]
BE32 = lambda b, o: struct.unpack(">I", b[o:o + 4])[0]
BE64 = lambda b, o: struct.unpack(">Q", b[o:o + 8])[0]

# HFSPlusCatalogFile record layout: dataFork starts 88 bytes into the record.
DATA_FORK_OFF = 88


def main():
    path, name = sys.argv[1], sys.argv[2]
    start = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
    limit = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0

    needle = name.encode("utf-16-be")
    print("image   %s" % path)
    print("name    %r  -> %d bytes utf-16be" % (name, len(needle)))
    print("needle  %s" % needle.hex())

    CHUNK = 1 << 24
    pos = start
    hits = 0
    with open(path, "rb") as f:
        f.seek(start)
        prev = b""
        while True:
            blk = f.read(CHUNK)
            if not blk:
                break
            buf = prev + blk
            base = pos - len(prev)
            i = buf.find(needle)
            while i != -1:
                abs_pos = base + i
                hits += 1
                print("\n--- hit at 0x%x ---" % abs_pos)
                if i >= 8:
                    keylen = BE16(buf, i - 8)
                    parent = BE32(buf, i - 6)
                    namelen = BE16(buf, i - 2)
                    print("  keyLength %d (expected %d), parentID %d, nameLen %d"
                          % (keylen, 4 + 2 + len(needle), parent, namelen))
                    rec = i + len(needle)
                    if rec % 2:
                        rec += 1
                    rtype = BE16(buf, rec)
                    fileid = BE32(buf, rec + 8)
                    print("  recordType %d (%s)  fileID %d"
                          % (rtype, "file" if rtype == 2 else "dir" if rtype == 1 else "?",
                             fileid))
                    if rtype == 2:
                        ls = BE64(buf, rec + DATA_FORK_OFF)
                        clump = BE32(buf, rec + DATA_FORK_OFF + 8)
                        tblk = BE32(buf, rec + DATA_FORK_OFF + 12)
                        print("  dataFork.logicalSize %d (0x%x)" % (ls, ls))
                        print("  dataFork.clumpSize   %d" % clump)
                        print("  dataFork.totalBlocks %d" % tblk)
                        for e in range(8):
                            eo = rec + DATA_FORK_OFF + 16 + 8 * e
                            sb = BE32(buf, eo)
                            bc = BE32(buf, eo + 4)
                            if sb == 0 and bc == 0:
                                break
                            print("    extent[%d] start=%d count=%d" % (e, sb, bc))
                i = buf.find(needle, i + 1)
                if limit and hits >= limit:
                    print("\n(hit limit reached)")
                    return
            prev = buf[-4096:]
            pos += len(blk)
    print("\ntotal hits: %d" % hits)


main()
