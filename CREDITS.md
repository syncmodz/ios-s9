# Credits

This project is an **AI-assisted hobby fork**. It exists only because of the
work below, and claims none of it. Huge thanks to every author.

## Foundations
- **ios-sweet** (dipa-studio) — the XNU + iOS 16.7 bring-up on a Qualcomm phone
  (Redmi Note 10 Pro) that this Galaxy S9 port started from. MIT.
- **ios-flame** (NWylynko) — the iOS 16 port to the Pixel 4. Its `METAL-NEXT.md`
  and `HANDOFF.md` are where the `ppnum_t` truncation in
  `pmap_sync_page_attributes_phys` and the display-engine autorefresh rule were
  found; both are in this tree now.
- **Veerhooda's iOS 12 / OnePlus 2 port** — `op2hfs.py`, the in-place HFS+
  reader/writer that `s9/tools/s9hfs.py` is derived from.
- **Apple** — XNU and the iOS userland. XNU is under the Apple Public Source
  License (APSL 2.0); the iOS userland is Apple's and is **not** redistributed
  here. No Apple binaries are included in this repository.

## Jailbreak userland (downloaded from official sources, not redistributed here)

The target is iOS 14.3, so the layer is **Taurine**, not palera1n: palera1n's
Procursus bootstrap starts at CFVER 1700 and there is no 1400 build at any URL.
Taurine is the jailbreak for iOS 14.0-14.3 and ships its own bootstrap.

- **Taurine** (Odyssey Team, CoolStar et al.) — the iOS 14 jailbreak, its
  bootstrap (`bootstrap.tar.gz`) and its basebinaries (`jailbreakd`, `jbexec`,
  `launchjailbreak`, `pspawn_payload.dylib`). 4-Clause BSD.
- **libhooker** (CoolStar) — the injection library Taurine uses. ElleKit is the
  iOS 15+ successor and is **not** used here.
- **Sileo** (Sileo Team, CoolStar et al.) — the package manager, 2.3 (the build
  Taurine ships).
- **Procursus** — the `apt`/`dpkg` lineage, and the 1700 suite for the plain CLI
  tools (OpenSSH). Not the bootstrap.
- **NewTerm** (Adam Demasi / HASHBANG Productions) — the terminal. GPLv3.
- **PreferenceLoader** (DHowett lineage) — Settings panels for tweaks.
- **OpenSSH** — via Procursus.
- **BigBoss** and the tweak repos — where tweaks and their dependencies come from.
- **ShowMyTouches** (dvntm) — used only to verify injection.

The iOS 16 tree in `xnu/patches/` used **palera1n**'s Procursus bootstrap
(CFVER 1800) and **ElleKit**; that layer is kept in git history and in
`s9/NOTES.md` as reference, not as the current path.

## Hardware reference
- **LineageOS / Samsung** SDM845 kernel sources (GPLv2) — the register sequences
  and device tree used to bring up the panel, touch, USB, RPMh, OSM, SMMU, etc.
- **U-Boot** (`qcom-hyp-smmu.c`, Linaro) — the apps-SMMU-behind-hypervisor recipe.

## This fork
- The Galaxy S9 shim, XNU patches, drivers and this jailbreak layer were written
  by a human + **Claude (Anthropic)** working together on real hardware.

Licenses of third-party components are their own; see each project. Nothing here
is affiliated with or endorsed by Apple, Samsung, or any of the above.
