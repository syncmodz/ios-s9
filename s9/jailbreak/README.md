# Galaxy S9 (SM-G9600) — iOS 14.3 jailbreak layer

This turns the XNU + **iOS 14.3 (18C66)** userland (booted by the shim, see `../README.md`
and `../NOTES.md`) into a usable, tweak-capable jailbreak: internet over the USB cable,
SSH, libhooker tweak injection, Sileo and NewTerm.

> **This is an AI-made fork.** The whole Galaxy S9 bring-up in this repository — the
> shim, the XNU patches, the device drivers, and this jailbreak layer — was built by a
> human directing an AI (Claude) step by step on real hardware. It is a hobby research
> port, provided as-is. See **CREDITS** below: it stands entirely on other people's work.

## Why Taurine and not palera1n

The iOS 16 tree used palera1n's Procursus bootstrap. **That does not exist for iOS 14**:

| what was tried | result |
| --- | --- |
| `static.palera.in/bootstrap-1400.tar.zst` | **404** — palera1n publishes only 1700 and 1800 |
| `apt.procurs.us/dists/1400/` | **404** — Procursus publishes only 1800, 1900, 2000, 3000 |
| ElleKit 1.2 | iOS 15+. It provides the Substrate slot, but it does not exist for 14.3 |
| Procursus `iphoneos-arm64/1700` pool | exists, but 1700 is the **iOS 15** suite; fine for plain CLI tools, not for a bootstrap |

**Taurine** (Odyssey Team) is the jailbreak for exactly this target: **iOS 14.0–14.3,
A8–A14**. It ships its own bootstrap inside the app (`Taurine/resources/bootstrap.tar.gz`,
13 MB, 2908 files, `iphoneos-arm`) instead of depending on Procursus, and its injection
layer is **libhooker**, not ElleKit.

Architecture is `iphoneos-arm` (arm64). The iPhone XR 14.3 userland is arm64, not arm64e,
so the arm64 slices are the ones that matter.

## How it fits together

1. `fetch-jailbreak.sh` downloads the third-party pieces from their **official sources**
   into `jb/`. This repo does **not** redistribute them.

   | piece | source | why |
   | --- | --- | --- |
   | `bootstrap.tar.gz` | `Odyssey-Team/Taurine` | the iOS 14 root: apt, dpkg, bash, zsh, coreutils |
   | `basebinaries.tar` | `Odyssey-Team/Taurine` | `jailbreakd`, `jbexec`, `launchjailbreak`, `pspawn_payload.dylib` |
   | Sileo 2.3 | `Odyssey-Team/Taurine` | the build Taurine itself ships (2.5.1 is the iOS 16 one) |
   | libhooker 1.6.9-1 + newabi | `repo.theodyssey.dev` | the injection library; `-newabi` is the `firmware >= 14.0` variant |
   | safemode, PreferenceLoader | `repo.theodyssey.dev` | libhooker's dependencies |
   | OpenSSH, NewTerm 3 | Procursus/Chariz | plain CLI tools, no firmware ceiling |

   It also takes this repo's own iOS pieces from its release (`s9net`, the `S9Wallpaper`
   tweak; source in `../ios-tools`) and the default wallpaper (`wallpaper/`).

2. `../tools/prep-ios6.sh` (run as root) builds the iOS root image: the Taurine bootstrap
   unpacked to `/`, Taurine's basebinaries in `/taurine`, the debs staged for first boot,
   our `s9net` (publishes the USB network to `configd`), the SSH key, and the launchd
   changes.

3. `../tools/jb-launchd.py` adds the boot jobs: `s9net`, `sshd`, and
   `com.ios-s9.respring` (one respring per boot, once SpringBoard and libhooker's injector
   are both up — the first SpringBoard of a boot starts before injection is ready); and
   sets `DYLD_INSERT_LIBRARIES=/usr/lib/TweakInject.dylib` on SpringBoard. It also adds
   `com.ios-s9.jbstart` for `jailbreakd`, but that is **off the critical path**: libhooker
   carries its own injector.

4. `../tools/trim-daemons.py` removes ~82 Apple daemons that only crash-loop and eat RAM
   on a device with no cellular / account / sensors / BT / Wi-Fi / GPU.

## What is different from the iOS 16 layer

- **Bootstrap**: Taurine's own, not Procursus 1800.
- **Injection**: libhooker. It does **not** use Taurine's `jailbreakd` — the
  package installs its own injector (`/etc/rc.d/libhooker` →
  `/usr/libexec/libhooker/inject_criticald`, with `libsyringe-14` for iOS 14)
  and its own `/usr/lib/TweakInject.dylib`. `jailbreakd` is downloaded and
  installed but is not on the boot path; see the caveat below.
- **Sileo**: 2.3, not 2.5.1.
- **Repos**: `ellekit.space` and `repo.palera.in` removed (both iOS 15+); `repo.theodyssey.dev`
  kept, Procursus `1700` added for CLI tools.
- **First boot**: `/prep_bootstrap.sh` from the Taurine bootstrap runs `dpkg --configure`
  on the staged packages, same as on a real device.

## The one thing to know before trusting this

On a real device Taurine **first applies a kernel exploit**, then unpacks the bootstrap and
starts `jailbreakd`. Here there is no exploit: the kernel is ours and already permissive
(`cs_enforcement_disable=1`, `launchd_unsecure_cache=1`, the `sweet_amfi.c` policy), so the
pieces are installed into the image ahead of time.

The good news, checked against the packages themselves: **libhooker does not need
Taurine's `jailbreakd`.** Its `data.tar` ships `/etc/rc.d/libhooker`, which starts
`/usr/libexec/libhooker/inject_criticald`, plus `libsyringe-14` (the iOS 14 injector) and
its own `/usr/lib/TweakInject.dylib`. That is a self-contained path — it wants a
permissive kernel, which is exactly what this one is.

`jailbreakd` is still downloaded and installed to `/taurine` and started by
`com.ios-s9.jbstart`, because it is what a real Taurine device runs and it may be needed by
packages that ask it for a port. But it is the piece whose authors assumed they had patched
the kernel, so it is the one most likely to crash. If it does, you will see it loop in
`jailbreakd.log` and libhooker should still work. `com.ios-s9.jbsetup` records the dpkg run
in `jbsetup.log`. Read both.

This is also downstream of the boot blocker: the GUI does not draw yet because
`containermanagerd` never gets a persona, which is *before* any jailbreak code runs.

## CREDITS

- **Taurine** (Odyssey Team, CoolStar et al.) — the iOS 14 jailbreak and its bootstrap.
  Not redistributed here; `fetch-jailbreak.sh` downloads it from the official repo.
- **libhooker** (CoolStar) — the injection library.
- **Sileo** (Sileo Team) — the package manager.
- **Procursus** — the 1700 suite for the CLI tools, and the `apt`/`dpkg` lineage.
- **NewTerm** (Adam Demasi / HASHBANG) — the terminal. GPLv3.
- **OpenSSH**, **PreferenceLoader** — their respective authors.

Licenses of third-party components are their own; see each project. Nothing here is
affiliated with or endorsed by Apple, Samsung, or any of the above.
