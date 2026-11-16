#!/usr/bin/env python3
"""Minimal HFS+/HFSX reader/writer for the on-device iOS root volume (S9).

Adapted from Veerhooda/iOS tools/op2hfs.py (OnePlus 2). The B-tree walking is
theirs; what changed for this device:

  * the device is /dev/block/sda29 (userdata), where the iOS root HFS+ image
    lives from offset 0 -- see ROOTFS-COMO-EDITAR-2026-11-12.md;
  * the catalog fork start block is READ FROM THE VOLUME HEADER instead of
    hard-coded (theirs had 23601 baked in for their image; ours differs);
  * a 3-level HFS+ extent walk is not needed here: our volume is 10 GiB and
    the catalog lives in the first extent, but the code still follows all
    eight extents the header gives, so it works either way.

Why this matters: the documented way to edit our rootfs is to read 10 GB to
the Mac, edit, and write 10 GB back. The Mac has ~5 GB free. This edits a
file's bytes on the partition in place through adb, never copying the volume.

LIMITS, same as the original: it cannot grow a file beyond its allocated
blocks, and it does not update the free-space bitmap. Use it to replace bytes
inside an existing file -- a plist, a dylib, a cache -- not to add files.

  op2hfs9.py ls <dirpath>
  op2hfs9.py cat <path> [off] [len]
  op2hfs9.py read <path> <off> <len> <out>
  op2hfs9.py write <path> <off> <infile>
  op2hfs9.py set_size <path> <size>

Requires ADB_LIBUSB=1 ADB_MDNS=0 in the environment (this Mac).
"""
import os
import struct
import subprocess
import sys
import tempfile

DEV = os.environ.get('S9_UFS_DEV', '/dev/block/sda29')
BS = 4096


def rdd(skip_blocks, count_blocks):
    """Read raw blocks from device; return exact bytes."""
    want = count_blocks * BS
    p = subprocess.run(['adb', 'exec-out',
                        'dd if=%s bs=%d skip=%d count=%d 2>/dev/null'
                        % (DEV, BS, skip_blocks, count_blocks)],
                       capture_output=True)
    if len(p.stdout) < want:
        raise RuntimeError('short read: got %d want %d' % (len(p.stdout), want))
    return p.stdout[:want]


def wrd(skip_blocks, data):
    """Write block-aligned bytes at a block offset, with fsync + readback."""
    assert len(data) % BS == 0
    tmp = None
    try:
        with tempfile.NamedTemporaryFile(delete=False) as t:
            t.write(data)
            tmp = t.name
        rpath = '/tmp/s9hfs_w.bin'
        p = subprocess.run(['adb', 'push', tmp, rpath], capture_output=True)
        if p.returncode != 0:
            raise RuntimeError('push failed: %s' % p.stderr.decode(errors='replace'))
        p = subprocess.run(['adb', 'shell',
                            'dd if=%s of=%s bs=%d seek=%d count=%d conv=notrunc,fsync 2>&1'
                            % (rpath, DEV, BS, skip_blocks, len(data) // BS)],
                           capture_output=True)
        out = p.stdout.decode(errors='replace')
        if p.returncode != 0 or 'records out' not in out.replace('\r', ''):
            raise RuntimeError('write failed: %s' % out)
        # Same discipline as flash.sh: read it back and compare.
        back = rdd(skip_blocks, len(data) // BS)
        if back != data:
            raise RuntimeError('READBACK MISMATCH at block %d -- do not reboot' % skip_blocks)
        subprocess.run(['adb', 'shell', 'rm -f %s; sync' % rpath], capture_output=True)
        return out
    finally:
        if tmp:
            os.unlink(tmp)


def u16(b, o):
    return struct.unpack('>H', b[o:o + 2])[0]


def u32(b, o):
    return struct.unpack('>I', b[o:o + 4])[0]


def parse_extents(b, o):
    out = []
    for i in range(8):
        s, c = struct.unpack('>II', b[o + i * 8:o + i * 8 + 8])
        if c:
            out.append((s, c))
    return out


def fork_len(b, o):
    return struct.unpack('>Q', b[o:o + 8])[0]


def volume_header():
    """The HFS+ volume header is the 512 bytes at offset 1024 of the volume."""
    vh = rdd(0, 1)[1024:1536]
    sig = struct.unpack('>H', vh[0:2])[0]
    if sig not in (0x482B, 0x4858):
        raise RuntimeError('not an HFS+ volume: sig 0x%04x (expected H+ 0x482b)' % sig)
    return vh


# HFSPlusVolumeHeader fork offsets. The fixed part is 112 bytes (2+2+4+4+4, then
# 4x4 dates, 2x4 counts, 3x4 block info, 4x4 clump/id, 4 writeCount, 8 encodings,
# 8x4 finderInfo), then five HFSPlusForkData of 80 bytes each. The original
# op2hfs.py hard-coded the catalog's first extent and never followed the fork;
# on our 10 GiB volume the catalog spans eight extents in the header and
# continues in the extents-overflow B-tree, so both are needed.
FORK_ALLOCATION = 0x070
FORK_EXTENTS = 0x0C0
FORK_CATALOG = 0x110
FORK_ATTRIBUTES = 0x160


class Fork:
    """An HFS+ fork: its starting extents from the header plus overflow records."""

    def __init__(self, vh, fork_off, file_id):
        self.size = fork_len(vh, fork_off)
        self.blocks = u32(vh, fork_off + 12)
        self.file_id = file_id
        self.extents = parse_extents(vh, fork_off + 16)
        self._extra = None

    def all_extents(self):
        """Header extents, then whatever the extents-overflow tree adds."""
        if self._extra is None:
            self._extra = extents_overflow(self.file_id)
        return self.extents + self._extra

    def blk(self, fb):
        """Device block of file block fb, walking the full extent list."""
        seen = 0
        for s, c in self.all_extents():
            if fb < seen + c:
                return s + (fb - seen)
            seen += c
        raise RuntimeError('file block %d past the fork extents' % fb)


# Special CNIDs (hfs_format.h). The extents-overflow key names the file by
# these, not by 0 -- the original op2hfs.py never read this tree at all.
CNID_EXTENTS = 3
CNID_CATALOG = 4
CNID_ALLOCATION = 6
CNID_STARTUP = 7
CNID_ATTRIBUTES = 8

_over = {}


def extents_overflow(file_id):
    """Read the extents-overflow B-tree records for one file.

    Key is HFSPlusExtentKey { u16 keyLength; u8 forkType; u8 pad; u32 fileID;
    u32 startBlock } -- keyLength 10, so the key body is 10 bytes. Records are
    HFSPlusExtentRecord (eight start/count pairs). Leaves are in startBlock
    order, so appending them in leaf order reconstructs the fork.
    """
    if file_id in _over:
        return _over[file_id]
    vh = volume_header()
    start = parse_extents(vh, FORK_EXTENTS + 16)[0][0]
    node_size = u16(rdd(start, 4), 32)
    per = max(1, node_size // BS)
    hdr = rdd(start, per)
    first_leaf = u32(hdr, 24)

    out = []
    n = first_leaf
    guard = 0
    while n != 0 and guard < 100000:
        guard += 1
        nd = rdd(start + n * per, per)
        nrec = u16(nd, 10)
        offs = rec_offs(nd)
        for i in range(nrec):
            rec = nd[offs[i]:offs[i + 1]]
            klen = u16(rec, 0)
            k = rec[2:2 + klen]
            if len(k) < 10:
                continue
            fid = u32(k, 2)
            if fid == file_id:
                out.extend(parse_extents(rec, 2 + klen))
        n = u32(nd, 0)
    _over[file_id] = out
    return out


class BTree:
    def __init__(self, fork):
        self.fork = fork
        hdr = rdd(fork.blk(0), 4)
        self.node_size = u16(hdr, 32)
        self.root = u32(hdr, 16)
        self.first_leaf = u32(hdr, 24)
        self.blk_per_node = max(1, self.node_size // BS)

    def blk(self, fb):
        return self.fork.blk(fb)

    def node(self, n):
        return rdd(self.blk(n * self.blk_per_node), self.blk_per_node)


CAT = None


def cat_init():
    global CAT
    if CAT is None:
        vh = volume_header()
        CAT = BTree(Fork(vh, FORK_CATALOG, CNID_CATALOG))
    return CAT


def rec_offs(nd):
    nrec = u16(nd, 10)
    L = len(nd)
    return [u16(nd, L - 2 * (i + 1)) for i in range(nrec + 1)]


def _key(rec):
    """(parentID, name-bytes) of a catalog key, or None if the record is too short.

    HFSPlusCatalogKey is { u16 keyLength; u32 parentID; HFSUniStr255 nodeName }.
    keyLength counts the key body only (not its own two bytes), so the body is
    rec[2:2+keyLength] and the name starts at offset 6 of it: 4 bytes of
    parentID, then the u16 name length, then UTF-16BE characters.
    """
    klen = u16(rec, 0)
    k = rec[2:2 + klen]
    if len(k) < 6:
        return None
    return (u32(k, 0), k[6:])


# HFS+ compares catalog keys with FastUnicodeCompare (see Apple's
# FastUnicodeCompare.c), NOT with memcmp. It is case-insensitive and skips a
# small set of ignorable characters. Sorting by raw UTF-16BE bytes puts
# 'ExtensibleSSO' before 'diagnostics' (0x45 < 0x64), which sends the B-tree
# descent into the wrong leaf -- measured: 'diagnostics' lives in node 22342
# while the byte-order descent went to 22350.
#
# A lower-case fold is enough to reproduce the ordering for the names on this
# volume; the ignorable-character set is applied too so the order matches.
_IGNORABLE = frozenset('\u0000\u0001\u0002\u0003\u0004\u0005\u0006\u0007'
                       '\u0008\u0009\u000a\u000b\u000c\u000d\u000e\u000f')


def _fold(s):
    """HFS+ sort weight of a UTF-16BE name, as a tuple of code points."""
    u = s.decode('utf-16-be', errors='replace')
    out = []
    for ch in u:
        if ch in _IGNORABLE:
            continue
        out.append(ch.lower() if ch.lower() else ch)
    return tuple(out)


def _kcmp(a, b):
    """-1/0/1 comparing two catalog keys (parentID first, then folded name)."""
    if a[0] != b[0]:
        return -1 if a[0] < b[0] else 1
    fa, fb = _fold(a[1]), _fold(b[1])
    return (fa > fb) - (fa < fb)


def find_record_loc(parent_id, name):
    """Walk the catalog B-tree; (node_num, record_offset, record_bytes excl. key) or None.

    The descent compares the FULL key (parentID, then the UTF-16BE name), not
    just the parentID. Comparing only the parentID picks the wrong child as
    soon as a directory has more than one index entry -- which is why
    /private/var/db/diagnostics resolved to a leaf that did not contain it
    while a full leaf-chain scan found it at leaf 22342.
    """
    bt = cat_init()
    target = (parent_id, name.encode('utf-16-be'))
    n = bt.root
    while True:
        nd = bt.node(n)
        kind = nd[8]
        nrec = u16(nd, 10)
        offs = rec_offs(nd)
        if kind == 0xFF:  # leaf
            for i in range(nrec):
                rec = nd[offs[i]:offs[i + 1]]
                k = _key(rec)
                if k is not None and _kcmp(k, target) == 0:
                    klen = u16(rec, 0)
                    return (n, offs[i], rec[2 + klen:])
            return None
        elif kind == 0x00:  # index
            child = None
            for i in range(nrec):
                rec = nd[offs[i]:offs[i + 1]]
                k = _key(rec)
                if k is not None and _kcmp(k, target) <= 0:
                    child = u32(rec, 2 + u16(rec, 0))
                else:
                    break
            if child is None:
                rec = nd[offs[0]:offs[1]]
                child = u32(rec, 2 + u16(rec, 0))
            n = child
        else:
            raise RuntimeError('unexpected node kind %d' % kind)


def find_record(parent_id, name):
    loc = find_record_loc(parent_id, name)
    return loc[2] if loc else None


def resolve_loc(path):
    """Absolute path -> (fileID, is_dir, data_extents, size, total_blocks, node, rec_off)."""
    parts = [p for p in path.split('/') if p]
    pid = 2  # root folder cnid
    for i, part in enumerate(parts):
        loc = find_record_loc(pid, part)
        if loc is None:
            raise RuntimeError('not found: %s (at %s)' % (path, part))
        n, rec_off, rec = loc
        rtype = u16(rec, 0)
        last = (i == len(parts) - 1)
        if rtype == 0x0001:  # folder
            pid = u32(rec, 8)
            if last:
                return (pid, True, [], 0, 0, n, rec_off)
        elif rtype == 0x0002:  # file
            fid = u32(rec, 8)
            if not last:
                raise RuntimeError('not a dir: %s' % part)
            size = fork_len(rec, 88)
            total_blocks = u32(rec, 88 + 12)
            exts = parse_extents(rec, 88 + 16)
            return (fid, False, exts, size, total_blocks, n, rec_off)
        else:
            raise RuntimeError('bad record type %d at %s' % (rtype, part))
    raise RuntimeError('empty path')


def resolve(path):
    fid, isdir, exts, size, _tb, _n, _ro = resolve_loc(path)
    return (fid, isdir, exts, size)


def file_blocks(exts, size):
    out = []
    for s, c in exts:
        out.extend(range(s, s + c))
    return out


def read_file(path, off, length):
    fid, isdir, exts, size = resolve(path)
    if isdir:
        raise RuntimeError('is a directory: %s' % path)
    if off + length > size:
        raise RuntimeError('read past EOF (%d > %d)' % (off + length, size))
    blocks = file_blocks(exts, size)
    start_blk = off // BS
    end_blk = (off + length + BS - 1) // BS
    blks = blocks[start_blk:end_blk]
    i = 0
    data = b''
    while i < len(blks):
        j = i
        while j + 1 < len(blks) and blks[j + 1] == blks[j] + 1:
            j += 1
        data += rdd(blks[i], j - i + 1)
        i = j + 1
    start_off = off - start_blk * BS
    return data[start_off:start_off + length]


def set_size(path, new_size):
    """Update the logical size in the catalog, within the allocated blocks."""
    fid, isdir, exts, size, total_blocks, n, rec_off = resolve_loc(path)
    if isdir:
        raise RuntimeError('is a directory: %s' % path)
    if new_size > total_blocks * BS:
        raise RuntimeError('cannot grow beyond allocated blocks (%d > %d)' % (new_size, total_blocks * BS))
    bt = cat_init()
    nd = bytearray(bt.node(n))
    klen = u16(nd, rec_off)
    size_off = rec_off + 2 + klen + 88
    struct.pack_into('>Q', nd, size_off, new_size)
    nb = bt.blk_per_node
    first = bt.blk(n * nb)
    if bt.blk(n * nb + nb - 1) == first + nb - 1:
        wrd(first, bytes(nd))
    else:
        for i in range(nb):
            wrd(bt.blk(n * nb + i), bytes(nd[i * BS:(i + 1) * BS]))
    return new_size


def write_file(path, off, payload):
    fid, isdir, exts, size, total_blocks, n, rec_off = resolve_loc(path)
    if isdir:
        raise RuntimeError('is a directory')
    target_size = max(size, off + len(payload)) if off > 0 else len(payload)
    if target_size != size:
        if target_size <= total_blocks * BS:
            print('[s9hfs] size %d -> %d (cap %d)' % (size, target_size, total_blocks * BS))
            set_size(path, target_size)
            size = target_size
        else:
            raise RuntimeError('write past EOF (%d > %d allocated)' % (target_size, total_blocks * BS))
    blocks = file_blocks(exts, size)
    first = off // BS
    last = (off + len(payload) - 1) // BS
    for bi in range(first, last + 1):
        blk = rdd(blocks[bi], 1)
        a = max(off, bi * BS) - bi * BS
        z = min(off + len(payload), (bi + 1) * BS) - bi * BS
        blk = blk[:a] + payload[bi * BS + a - off:bi * BS + z - off] + blk[z:]
        wrd(blocks[bi], blk)


def ls_dir(path):
    parts = [p for p in path.split('/') if p]
    pid = 2
    for part in parts:
        rec = find_record(pid, part)
        if rec is None or u16(rec, 0) != 0x0001:
            raise RuntimeError('no such dir: %s' % path)
        pid = u32(rec, 8)
    bt = cat_init()
    n = bt.first_leaf
    out = []
    while n != 0:
        nd = bt.node(n)
        nrec = u16(nd, 10)
        offs = rec_offs(nd)
        for i in range(nrec):
            rec = nd[offs[i]:offs[i + 1]]
            klen = u16(rec, 0)
            k = rec[2:2 + klen]
            if u32(k, 0) == pid:
                nl = u16(k, 4)
                nm = k[6:6 + nl * 2].decode('utf-16-be', errors='replace')
                body = rec[2 + klen:]
                rt = u16(body, 0)
                if rt in (1, 2):
                    out.append(('d' if rt == 1 else 'f', nm))
        n = u32(nd, 0)  # fLink
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    a = sys.argv[1]
    if a == 'ls':
        for t, n in ls_dir(sys.argv[2]):
            print(t, n)
    elif a == 'cat':
        p = sys.argv[2]
        off = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
        ln = int(sys.argv[4], 0) if len(sys.argv) > 4 else 64
        sys.stdout.buffer.write(read_file(p, off, ln))
    elif a == 'read':
        p, off, ln, out = sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0), sys.argv[5]
        open(out, 'wb').write(read_file(p, off, ln))
        print('wrote %s' % out)
    elif a == 'write':
        p, off, inf = sys.argv[2], int(sys.argv[3], 0), sys.argv[4]
        data = open(inf, 'rb').read()
        write_file(p, off, data)
        print('wrote %d bytes at %s+%s' % (len(data), p, sys.argv[3]))
    elif a == 'set_size':
        p, sz = sys.argv[2], int(sys.argv[3], 0)
        print('set %s logical size to %d' % (p, set_size(p, sz)))
    else:
        sys.exit('unknown cmd')


if __name__ == '__main__':
    main()
