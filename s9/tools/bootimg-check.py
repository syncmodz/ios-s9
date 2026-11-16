#!/usr/bin/env python3
"""Check a boot.img v0 the way build.sh makes it, without trusting the name.

  bootimg-check.py IMAGE [--extract DIR]

Prints the header, verifies the id (SHA-1 over kernel, ramdisk, second, as
AOSP mkbootimg writes it), splits the kernel into Image.gz + appended DTBs,
gunzips the Image and reads its arm64 header, and lists the ramdisk's cpio
entries. Exit status 1 on anything malformed. --extract writes Image.gz-dtb,
Image, dtbs.bin and ramdisk.cpio.gz to DIR (to rebuild and compare).
"""
import gzip, hashlib, os, struct, sys, zlib

def sha(b):
    return hashlib.sha256(b).hexdigest()

def fail(msg):
    print('ERROR: ' + msg)
    sys.exit(1)

args = sys.argv[1:]
out = None
if '--extract' in args:
    i = args.index('--extract')
    out = args[i + 1]
    del args[i:i + 2]
img = open(args[0], 'rb').read()
if img[:8] != b'ANDROID!':
    fail('no ANDROID! magic')
(ksize, kaddr, rsize, raddr, ssize, saddr, tags, page, hver, osv) = struct.unpack_from('<10I', img, 8)
name = img[48:64].rstrip(b'\0').decode()
cmdline = img[64:576].rstrip(b'\0').decode()
img_id = img[576:608]
extra = img[608:1632].rstrip(b'\0').decode()
osver = osv >> 11
patch = osv & 0x7ff
print(f'file      {args[0]} ({len(img)} bytes) sha256 {sha(img)}')
print(f'header    v{hver} page {page} base-relative: kernel 0x{kaddr:08x} ramdisk 0x{raddr:08x} '
      f'second 0x{saddr:08x} tags 0x{tags:08x}')
print(f'os        {osver >> 14}.{(osver >> 7) & 0x7f}.{osver & 0x7f} patch {2000 + (patch >> 4)}-{patch & 0xf:02d} '
      f'board {name!r} cmdline {cmdline!r} extra {extra!r}')
if hver != 0:
    fail(f'header v{hver}, build.sh makes v0')

def pages(n):
    return (n + page - 1) // page

koff = page
roff = koff + pages(ksize) * page
soff = roff + pages(rsize) * page
end = soff + pages(ssize) * page
kernel = img[koff:koff + ksize]
ramdisk = img[roff:roff + rsize]
second = img[soff:soff + ssize]
if end > len(img):
    fail(f'image truncated: needs {end} bytes')
tail = img[end:]
print(f'layout    kernel {ksize} ramdisk {rsize} second {ssize}, {len(tail)} bytes after '
      f'({"all zero" if tail.count(0) == len(tail) else "NOT zero"})')

h = hashlib.sha1()
for part in (kernel, ramdisk, second):
    h.update(part)
    h.update(struct.pack('<I', len(part)))
print(f'id        {"ok" if h.digest() == img_id[:20] else "MISMATCH"} ({img_id[:20].hex()})')
if h.digest() != img_id[:20]:
    fail('id mismatch')

# kernel = Image.gz followed by the stock DTBs (build.sh: cat Image.gz dtbs.bin)
d = zlib.decompressobj(16 + zlib.MAX_WBITS)
image = d.decompress(kernel) + d.flush()
if not d.eof:
    fail('Image.gz does not end')
dtbs = d.unused_data
gzlen = len(kernel) - len(dtbs)
print(f'Image.gz  {gzlen} bytes -> Image {len(image)} bytes sha256 {sha(image)}')
code0, code1, text_off, image_size, flags, r2, r3, r4, magic, pe = struct.unpack_from('<IIQQQQQQII', image, 0)
print(f'arm64 hdr text_offset 0x{text_off:x} image_size 0x{image_size:x} ({image_size}) flags 0x{flags:x} '
      f'magic {struct.pack("<I", magic)!r}')
if magic != 0x644d5241:
    fail('no ARM\\x64 magic in the Image header')
if image_size != len(image):
    print(f'WARNING   image_size {image_size} != Image length {len(image)}')
n = 0
off = 0
ids = []
while off < len(dtbs):
    m, total = struct.unpack_from('>II', dtbs, off)
    if m != 0xd00dfeed:
        break
    n += 1
    off += total
rest = len(dtbs) - off
print(f'dtbs      {n} FDTs, {off} bytes sha256 {sha(dtbs[:off])}, then {rest} bytes of non-FDT data')

rd = gzip.decompress(ramdisk) if ramdisk else b''
entries = []
p = 0
while p + 110 <= len(rd) and rd[p:p + 6] in (b'070701', b'070702'):
    namesize = int(rd[p + 94:p + 102], 16)
    filesize = int(rd[p + 54:p + 62], 16)
    nm = rd[p + 110:p + 110 + namesize - 1].decode()
    entries.append(nm)
    p = (p + 110 + namesize + 3) & ~3
    p = (p + filesize + 3) & ~3
    if nm == 'TRAILER!!!':
        break
print(f'ramdisk   {rsize} bytes gz -> {len(rd)} bytes cpio, {len(entries)} entries'
      f'{": " + ", ".join(entries[:6]) if entries else ""}{" ..." if len(entries) > 6 else ""}')
print(f'          sha256 gz {sha(ramdisk)}')

if out:
    os.makedirs(out, exist_ok=True)
    for fn, data in (('Image.gz-dtb', kernel), ('Image.gz', kernel[:gzlen]), ('Image', image),
                     ('dtbs.bin', dtbs), ('ramdisk.cpio.gz', ramdisk), ('second.bin', second)):
        open(os.path.join(out, fn), 'wb').write(data)
    print(f'extracted to {out}')
