# Reference sources

The projects this port learned from, and what each one actually teaches. This is
the reading list, not a copy of the code: every entry links to its own
repository, and `clone-refs.sh` fetches them all.

**Why links and not the code.** The working set is 3.5 GB, most of it third-party
code that is already published upstream. Four of these projects ship **no
license at all** (`Bergamota`, `ios-topaz`, `ios-sailfish`, `hakios-sm8150`),
which under copyright means all rights reserved — they cannot be redistributed
here. The rest are MIT, GPL or APSL and could be, but copying them would only
make this repository a stale mirror of someone else's work.

**How to read this.** Each entry says what the project is, and then — the part
that matters — **what it taught us**, with the specific file or finding. If you
are starting your own port, the "what it teaches" column is the shortest path to
not repeating our mistakes.

---

## The ports, in order of how much they gave us

### ios-sweet — `dipa-studio/ios-sweet` · MIT · 2.0 MB

**What it is.** The origin of this whole family: Apple's open-source XNU on a
Qualcomm phone (Redmi Note 10 Pro, SM7150), running the unmodified iOS 16
userland. Every port below, including this one, is a fork of it.

**What it teaches.** The base architecture, and it is the one to read first:

- `xnu/patches/` — 84 patches, the foundation series. The build-system work
  (`config/MASTER.arm64.sweet`, `makedefs/`), the arm64 machine bring-up, and
  the `sweet_*` driver convention that every fork keeps.
- `shim/` — the bare-metal boot shim, and the `m1`…`m12` staging scheme that
  makes a boot diagnosable without a serial port.
- `tools/` — the device-tree builder (`mkadt.py`), the payload packer, the log
  reader. Ours are descendants of these.
- `NOTES.md` — the engineering log. Worth reading for the *shape* of the
  problems, not the answers: it is a different SoC.

### ios-flame — `NWylynko/ios-flame` · 5.7 MB

**What it is.** ios-sweet ported to the Pixel 4 (SM8150, Adreno 640), iOS 16.7.16,
with **Metal working** — the lock screen, home screen, Settings, Control Center
and Safari all render through Metal on the Adreno.

**What it teaches.** This is the most valuable single reference for anyone
chasing graphics, and two of its findings are already in our kernel:

- **`METAL-NEXT.md` §4, item 1 — the `ppnum_t` truncation.** `pmap_sync_page_attributes_phys`
  did `flush_dcache((vm_offset_t)(pp << PAGE_SHIFT), ...)`, and `ppnum_t` is 32
  bits, so any physical address at or above 4 GB flushes the wrong cache line.
  On our S9 that silently affected **2018 MB of RAM**. Fixed here as `ptoa(pp)`.
  This is the single highest-value line in the whole reference set.
- **`METAL-NEXT.md` §3** — how UniXNU does native Metal, and §5 the plan for a
  new GPU. Read this before designing a GPU path.
- `HANDOFF.md`, `RECOVERY.md` — the operational discipline: how to hand a port
  over, and how to get the phone back when a flash goes wrong.

### Veerhooda's iOS 12 / OnePlus 2 — `Veerhooda/iOS`, `Veerhooda/xnu` · 3.3 MB + 83 MB

**What it is.** iOS 12.5.7 on a OnePlus 2 (MSM8994, Snapdragon 810), booting from
Qualcomm's own bootloader into a shim, with a real HFS+ root on eMMC.

**What it teaches.** The **HFS+ tooling**, which is why it is here:

- `tools/op2hfs.py` — an in-place HFS+ editor. Our `s9hfs.py` is derived from it.
  If you need to read or write an iOS root filesystem from a host without
  mounting it, start here.
- `tools/stage-hfs.sh` — how to stage a root image reproducibly.
- `docs/AUDIT-2026-11-11.md` — an honest audit of its own older milestone claims.
  A good model for how to write down what you actually verified.

### ios-sailfish — `devilAPI/ios-sailfish` (fork of dipa-studio) · 2.8 MB

**What it is.** iOS 16.7.16 on a Google Pixel 1 (Snapdragon 821), **with GPU
acceleration**: CoreAnimation's OpenGL ES renderer on Mesa's freedreno, through
its own kernel GPU driver. Display up to 60 Hz, double-buffered, no tearing.

**What it teaches.** The complete GPU bring-up sequence for an Adreno, in order,
as 187 patches. The `0119`–`0128` series is the one to read:

| patch | what it establishes |
| --- | --- |
| `0119-gpu-power-probe` | how to find out whether the GPU is even powered |
| `0121-gpu-smmu-identity` | SMMU in identity mode — the trick that unblocks DMA before you understand the SMMU |
| `0124-gpu-mmio-no-wzr` | MMIO without write-combining, when the bus does not tolerate it |
| `0126-gpu-2d-blit` | a 2D blit path, enough to prove the pipeline works |
| `0127-gpu-rbccu-sp-power` | the RBCCU/SP power domain, which is where Adreno bring-up usually stalls |

Its README is also the most honest status table in the set: it says exactly what
works (GPU, display, Wi-Fi WPA2) and what does not (no Metal, no WPA3).

### ios-topaz — `MakrSas/ios-topaz` · 3.4 MB

**What it is.** iOS 16.7.16 on a Redmi Note 12 4G (SM6225 "khaje", Adreno 610).
204 patches — the largest series here.

**What it teaches.** Two things the others do not:

- **The first-pixels milestone, documented in detail.** `NOTES.md` records the
  exact console lines that prove backboardd drew through the kernel display
  path: `SWEET SURF: backboardd create id 2/3 1080x2400 fmt 0x42475241 (BGRA)`,
  the IOSurface selectors, the IOMFB `SwapBegin`/`SwapEnd` traps, and the
  alternating `SWEET FB: swap N layer 0 surface 2|3` that proves double
  buffering. If you are trying to tell "the display works" from "the display
  looks like it works", this is the reference.
- **The `0901`–`0911` GPU series** — memory from the upper DDR bank, 36-bit
  addressing, GPU kernel buffers from ordinary pages, embedded firmware step
  markers, and a 64 MB-chunk GPU pool. This is the series to read when the GPU
  powers on but faults on memory.

### ios-apollo — `enesuzun2002/hakios-apollo` · MIT · 4.1 MB

**What it is.** iOS on a Samsung Galaxy S20 (SM8250, Adreno 650), 143 patches.

**What it teaches.** The **GPU power and TrustZone path**, which is the part
that differs most between SoCs:

- `xnu/hakios.patch` — the kernel side as one patch, applied by `xnu/apply.sh`
  (this project does not use a numbered series like the others).
- The **GPU power and TrustZone path**: GPU probe, CX/GMU power,
  EPSS/CPUFreq/SMMU interaction, hypervisor-safe register access, HFI GX power,
  SQE/CP start, vendor SQE, core stream device tree, and **zap shader loading
  through TrustZone**. The zap shader is the step that stops most Adreno
  bring-ups, and this is the clearest treatment of it in the set.
- `wifi/` — a separate Wi-Fi bring-up tree, the only one here.

### Bergamota — `ry-diffusion/Bergamota` · **no license, read only** · 21 MB

**What it is.** iOS 18.7.8 on a Poco F1 (Snapdragon 845 — **the same SoC family
as our S9**), to SpringBoard, with its own kernel ("Heartwood").

**What it teaches.** The closest thing to our target, and the reason it is
listed despite having no license: **read it, do not copy it.** The Snapdragon
845 specifics — UFS, the display controller, the power domains — are the same
silicon we are driving. Its `tools/identity/` and the four-part structure
(kernel, userland, drivers, tooling) are a good model for organising a port.

### Chlorophyll — `ry-diffusion/Chlorophyll` · 4.1 MB

**What it is.** Bergamota's Metal driver: it translates Apple's AIR (the GPU IR
inside `.metallib` files) to Mesa's NIR, compiles it with freedreno's ir3
backend, and implements the Metal API over Mesa's Gallium driver.

**What it teaches.** The only complete answer in this set to "how do I run Metal
without Apple's GPU driver". If the S9 ever gets past the CPU-rendering stage,
this is the path. `air/` is the AIR→NIR frontend; the rest is the Metal API
surface.

### metal2vulkan — `steelbrain/metal2vulkan` · GPL-2.0+ / LGPL-3.0+ · 1.6 GB

**What it is.** Runs Metal shaders and Metal programs on Vulkan: reads Metal AIR
and compiles it to Vulkan pipelines, and offers the Metal API on top of a Vulkan
device.

**What it teaches.** The alternative to Chlorophyll: instead of writing a Metal
driver, translate Metal to Vulkan and use the phone's existing Vulkan driver.
Larger and more general, and the `crates/` tree is most of the 1.6 GB.

### iOSPlus10 — `MellysInformejtik/slop` · 9.4 MB

**What it is.** iOS on a OnePlus 10 Pro (SM8450), booted through UEFI, aiming at
iOS 26 with Metal.

**What it teaches.** The **arm64e / PAC** problem. Its cores have ARMv8.3 PAC, so
the arm64e iOS 26 userland runs without the shared-cache rewrite that phones
without PAC need. If you are targeting iOS 15+, read this before assuming you
need to rewrite the dyld shared cache.

### HakiOS — `oosexgang/hackios-xnu-kernel` · 186 MB

**What it is.** XNU (xnu-8796) with its own bootloader and drivers on a Qualcomm
phone, booting stock iOS 16.5 userspace — and the same thing in QEMU.

**What it teaches.** **The QEMU path.** Nothing is flashed: the loader goes in
through `fastboot boot` and the system lives in RAM. That is a far faster
iteration loop than flashing, and the QEMU target means you can debug the kernel
without a phone at all. `hakios-sm8150` (POCO X3 Pro) is the device/SoC layer
and shows how to structure one.

### Portsmith-iOS — `Chromitsune/Portsmith-iOS` · MIT · 37 MB

**What it is.** A workbench for XNU/iOS bring-up on non-Apple ARM64 devices: not
a port itself, but the tooling and the catalogue around ports.

**What it teaches.** **The map.** `sources/manifest.toml` lists **115** port and
framework sources with pinned revisions, and `tools/checkout-sources.py` fetches
a curated 12 or any named one. If you want to find a port to a device close to
yours, start here rather than searching GitHub.

### klte-ios7 — 2.5 MB

**What it is.** iOS 7.1.2 on a Galaxy S5 (MSM8974, Snapdragon 801) — an ARMv7
port of XNU 2107.

**What it teaches.** That the same approach works on 32-bit ARM and on much older
iOS. Useful if you are targeting an old device, and a reminder that the
technique is not tied to arm64 or to iOS 16.

### Taurine — `Odyssey-Team/Taurine` · 30 MB

**What it is.** The iOS 14.0–14.3 jailbreak (A8–A14). Not a port — it is the
jailbreak layer this project uses.

**What it teaches.** `Taurine/post-exploit/bootstrap.swift` is the authoritative
answer to "is Taurine rootful or rootless": it runs
`tar -xkf /taurine/bootstrap.tar -C /`, i.e. **rootful**, to the filesystem root.
`Taurine/resources/` holds the bootstrap and basebinaries we install.

### The kernel and the SoC

- **`apple-oss-distributions/xnu`** (APSL 2.0) — the kernel itself. We run
  **xnu-7195** (iOS 14.3); the iOS 16 tree used xnu-8796.
- **`android_kernel_samsung_sdm845-lineage-20`** (GPLv2, 890 MB) — LineageOS's
  SDM845 kernel. The register sequences and device tree for the panel, touch,
  USB, RPMh, OSM and SMMU all came from here. It is the single most useful
  source for "what value goes in this register", and the reason the S9 port was
  feasible at all.

---

## What we took from each, in one line

| source | the one thing |
| --- | --- |
| ios-sweet | the architecture, and the `sweet_*` driver convention |
| ios-flame | **the `ppnum_t` truncation** — 2018 MB of our RAM was affected |
| Veerhooda | `op2hfs.py`, the in-place HFS+ editor our `s9hfs.py` descends from |
| ios-sailfish | the ordered Adreno bring-up, and SMMU identity mode |
| ios-topaz | how to *prove* the display path works, from the console lines |
| ios-apollo | the zap shader through TrustZone — where Adreno bring-up stalls |
| Bergamota | the same SoC family (SDM845), read-only |
| Chlorophyll | AIR→NIR, the only complete "Metal without Apple" answer |
| metal2vulkan | the other route: Metal on top of Vulkan |
| iOSPlus10 | arm64e/PAC, and when you do *not* need a cache rewrite |
| HakiOS | QEMU and `fastboot boot` — iterate without flashing |
| Portsmith | the catalogue: 115 ports, pinned |
| klte-ios7 | it works on ARMv7 and old iOS too |
| Taurine | rootful, proven from `bootstrap.swift` |
| LineageOS SDM845 | every register value we needed |

## Licensing, plainly

| license | sources |
| --- | --- |
| MIT | ios-sweet, ios-apollo, Portsmith-iOS |
| GPL-2.0+ / LGPL-3.0+ | metal2vulkan |
| GPLv2 | LineageOS SDM845 kernel |
| APSL 2.0 | Apple's XNU |
| own terms | Taurine, Chlorophyll, HakiOS, iOSPlus10, klte-ios7 |
| **none stated** | **Bergamota, ios-topaz, ios-sailfish, hakios-sm8150** |

The four with no license are all rights reserved by default. They are linked
here, not copied, and that is deliberate — do not vendor them into your own
repository without asking their authors.

## Using this

```sh
./clone-refs.sh              # everything, ~3.5 GB
./clone-refs.sh --small      # only the ones under 10 MB, ~60 MB
./clone-refs.sh ios-flame    # one by name
```

It clones into `refs/src/`, which is gitignored.
