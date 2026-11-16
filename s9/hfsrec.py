#!/usr/bin/env python3
"""Dump the full HFS+ catalog record for a file name found inside an image.

The kernel reports a nonsense va_data_size for /sbin/launchd while the disk
record below is the truth. The field that decides which branch hfs_vnop_getattr
takes is the BSD flags word: if UF_COMPRESSED (0x20) is set, getattr reports the
*uncompressed* size read from the decmpfs header instead of the catalog
logicalSize, and hfs_vnop_read diverts to decmpfs_read_compressed. That would
explain "absurd size + all-zero bytes" in one shot, so it is worth printing.

  hfsrec.py <image> <name>
"""
import struct
import sys

BE16 = lambda b, o: struct.unpack(">H", b[o:o + 2])[0]
BE32 = lambda b, o: struct.unpack(">I", b[o:o + 4])[0]
BE64 = lambda b, o: struct.unpack(">Q", b[o:o + 8])[0]

# HFSPlusCatalogFile field offsets from the start of the record.
F_RECTYPE = 0    # 2
F_FLAGS = 2      # 2
F_FILEID = 8     # 4
F_PERM = 32      # HFSPlusBSDInfo: ownerID 4, groupID 4, adminFlags 1,
                 #                 ownerFlags 1, fileMode 2, special 4
F_USERINFO = 48
F_FINDERINFO = 64
F_TEXTENC = 80
F_DATAFORK = 88
F_RSRCFORK = 168

UF_COMPRESSED = 0x20
UF_IMMUTABLE = 0x01
UF_APPEND = 0x04


def fork(buf, off, label):
    logical = BE64(buf, off)
    clump = BE32(buf, off + 8)
    total = BE32(buf, off + 12)
    print("  %-12s logicalSize=%-12d clump=%-6d totalBlocks=%d"
          % (label, logical, clump, total))
    for e in range(8):
        eo = off + 16 + 8 * e
        sb = BE32(buf, eo)
        bc = BE32(buf, eo + 4)
        if sb == 0 and bc == 0:
            break
        print("      extent[%d] start=%-10d count=%d" % (e, sb, bc))
    return logical, total


def main():
    path, name = sys.argv[1], sys.argv[2]
    needle = name.encode("utf-16-be")
    CH = 1 << 24
    found = 0
    with open(path, "rb") as f:
        pos = 0
        prev = b""
        while True:
            blk = f.read(CH)
            if not blk:
                break
            buf = prev + blk
            base = pos - len(prev)
            i = buf.find(needle)
            while i != -1:
                ko = i - 8
                if ko >= 0:
                    keylen = BE16(buf, ko)
                    parent = BE32(buf, ko + 2)
                    nlen = BE16(buf, ko + 6)
                    if keylen == 4 + 2 + len(needle) and nlen == len(needle) // 2:
                        rec = ko + 2 + keylen
                        if rec % 2:
                            rec += 1
                        rtype = BE16(buf, rec)
                        if rtype in (1, 2):
                            found += 1
                            print("=" * 66)
                            print("record at 0x%x  parentID=%d  type=%s"
                                  % (base + ko, parent,
                                     "file" if rtype == 2 else "folder"))
                            if rtype == 2:
                                fileid = BE32(buf, rec + F_FILEID)
                                owner = BE32(buf, rec + F_PERM)
                                group = BE32(buf, rec + F_PERM + 4)
                                admin = buf[rec + F_PERM + 8]
                                ownerf = buf[rec + F_PERM + 9]
                                mode = BE16(buf, rec + F_PERM + 10)
                                flags16 = (admin << 8) | ownerf
                                print("  fileID       %d" % fileid)
                                print("  uid/gid      %d/%d  mode %04o" % (owner, group, mode))
                                print("  adminFlags   0x%02x   ownerFlags 0x%02x"
                                      % (admin, ownerf))
                                print("  bsdFlags     0x%04x  %s"
                                      % (flags16,
                                         " **UF_COMPRESSED SET**" if flags16 & UF_COMPRESSED
                                         else "(not compressed)"))
                                d_ls, d_tb = fork(buf, rec + F_DATAFORK, "dataFork")
                                r_ls, r_tb = fork(buf, rec + F_RSRCFORK, "rsrcFork")
                                if r_ls or r_tb:
                                    print("  NOTE: resource fork present -> decmpfs"
                                          " payload usually lives here")
                i = buf.find(needle, i + 1)
            prev = buf[-4096:]
            pos += len(blk)
    print("\nvalid records found: %d" % found)


main()
