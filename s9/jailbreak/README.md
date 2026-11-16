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

## What was checked, and what is still open

Every claim below was verified against the actual binaries, not assumed.

### Verified

| check | result |
| --- | --- |
| All 11 download URLs | **200**, sizes match |
| Architecture of every deb | `iphoneos-arm` (arm64) — what the iPhone XR 14.3 userland is |
| Taurine bootstrap unpacks | yes: `usr/bin/dpkg`, `usr/bin/apt`, `prep_bootstrap.sh` |
| Sileo's deps vs the bootstrap | `coreutils 8.32-10` ✓, `dpkg 1.20.9-1` ✓, `apt 2.3.3` ✓, `libzstd1 1.4.9-1` ✓ |
| `firmware` / `cy+cpu.arm64` virtual packages | provided by dpkg itself; `firmware-sbin` is installed with `Pre-Depends: firmware (>= 11.0)`, which proves it |
| libhooker's injector | `/etc/rc.d/libhooker` → `inject_criticald` (minos 13.7) + `libsyringe-14` + `/usr/lib/TweakInject.dylib` |
| `libsyringe-14` minos | **14.4** — see below |
| Sileo's Swift linkage | 8 **hard** links (`libswiftCore`, `Foundation`, `UIKit`, `ObjectiveC`, `Dispatch`, `Darwin`, `CoreGraphics`, `CoreFoundation`) |
| iOS 14.3's Swift runtime | only `libswiftDemangle.dylib` and `libswiftXCTest.dylib` in `/usr/lib/swift` — **not enough** |
| `org.swift.libswift 5.0-electra2` | ships all 8 in `/usr/lib/libswift/stable`, which is already on Sileo's `LC_RPATH`; `libswiftCore` is `LC_VERSION_MIN_IPHONEOS 7.0` |
| Does the kernel enforce `minos`? | **no** — `mach_loader.c` only reads `LC_BUILD_VERSION` to detect simulator binaries, and only on x86_64 |
| Does dyld-852.2 enforce `minos`? | **no** — it parses it for version APIs and dylib compat checks, never compares it to the OS version |

### The rootfs already has this bootstrap — do not install a second one

The 14.3 root image is **not** a bare Apple userland. It already carries the
Taurine bootstrap, and it is the same one this script downloads:

| check | result |
| --- | --- |
| `/Library/dpkg/status` | **58 packages, byte-identical** to Taurine's `bootstrap.tar.gz` (sha256 `2748249d…` on both) |
| package set and versions | **zero differences** — same 58 names, same versions |
| `/usr/bin/dpkg` | sha256 `27b05180…` on both |
| `/Applications/Sileo.app/Sileo` | sha256 `4ce7f8fd…` on both — Sileo 2.3 is already installed |
| `/taurine/` | already populated: `jailbreakd`, `jbexec`, `launchjailbreak`, `amfidebilitate`, `ctbypass`, `pspawn_payload*.dylib`, `uicache` |

So `prep-ios6.sh` unpacking `bootstrap.tar.gz` over it is a no-op at best and a
risk at worst (it would rewrite 2908 files and the dpkg database for no gain).

**What is actually missing** from the rootfs, and therefore the only work left:

| missing | why it matters |
| --- | --- |
| `/usr/lib/libswift/stable/*` | Sileo links 8 Swift dylibs hard; 14.3 ships only `libswiftDemangle`/`libswiftXCTest` |
| `/usr/libexec/libhooker/*`, `/usr/lib/TweakInject.dylib` | the injection layer itself — not installed |
| `/etc/apt/sources.list.d/` | no repos configured, so Sileo has nothing to list |
| `/usr/sbin/sshd`, `/usr/libexec/sshd-keygen-wrapper` | OpenSSH not installed |
| `/usr/libexec/s9net` | our own USB-network publisher |

That is a much smaller job than "build a bootstrap": install `libswift`, install
`libhooker` + `safemode` + `preferenceloader`, write the sources, install
OpenSSH, drop in `s9net`. The bootstrap step should be **skipped when
`/Library/dpkg/status` already exists**.

### Is Taurine rootful or rootless?

**Rootful.** Verified from the source, not assumed:

- `bootstrap.swift` runs `tar -xkf /taurine/bootstrap.tar -C /` — it unpacks to
  the **root of the filesystem**, not to `/var/jb`.
- `basebinaries` go to `/taurine`, and `jailbreakd` is started from there.
- The bootstrap tarball contains **zero** paths under `/var/jb`; `dpkg` lands at
  `/usr/bin/dpkg`.
- The marker it writes is `/.installed_taurine`, at the root.

Taurine is an iOS 14 jailbreak, and rootless jailbreaks (`/var/jb`) only became
the norm with iOS 15. So the whole layer here is rootful — which is why
NewTerm's rootless build cannot work, and why the rootfs's `/usr/bin/dpkg` is
the right place for it.

### libhooker — audited

| check | result |
| --- | --- |
| `data.tar` contents | `/etc/rc.d/libhooker`, `/usr/libexec/libhooker/{inject_criticald,libsyringe,libsyringe-13,libsyringe-14}`, `/usr/lib/TweakInject.dylib`, `/Library/TweakInject/` |
| minos | `inject_criticald` 13.7, `libsyringe` 13.7, `libsyringe-13` 13.7, **`libsyringe-14` 14.4** |
| `libhooker.dylib` / `TweakInject.dylib` (newabi) | minos 14.0, arm64 + arm64e |
| system deps | `libc++.1.dylib`, `libobjc.A.dylib`, `libSystem.B.dylib`, Foundation, CoreFoundation — all present in the 14.3 rootfs |
| `inject_criticald` needs | `mach_vm_write`/`mach_vm_protect` on a foreign task, i.e. `task_for_pid` |
| `task_for_pid` as root | **passes without entitlements.** `task_for_pid_posix_check` in `bsd/vm/vm_unix.c` returns TRUE for `kauth_cred_issuser`, before any other test |
| MACF gate | `mac_proc_check_get_task` uses `MAC_CHECK`, which starts at 0; no policy in this tree implements `mpo_proc_check_get_task`, so it returns 0 (allow) |
| libhooker selection | `libsyringe` compares `kCFCoreFoundationVersionNumber` against 1664.0 and picks `libsyringe-14` above it; 14.3 is above it |

So libhooker's injection path is expected to work: it runs as root, root passes
`task_for_pid`, and no MAC policy blocks it. The one unknown is `libsyringe-14`'s
14.4 build, as above.

### NewTerm — use 2.5, not 3.0

NewTerm **3.0~beta1** cannot work here, for two independent reasons:

1. It links `libswiftUniformTypeIdentifiers.dylib` **hard**. That dylib does not
   exist on iOS 14.3 — `UniformTypeIdentifiers` arrived in 14.5, and the
   framework on this rootfs has only `Info.plist` and `_CodeSignature`, no
   binary. `org.swift.libswift` cannot supply it; it predates the module.
2. It is the **rootless** build: it spawns `/var/jb/usr/bin/login`, and a
   rootful jailbreak has no `/var/jb`.

NewTerm **2.5** is the right one, and it checks out:

| check | result |
| --- | --- |
| `LC_VERSION_MIN_IPHONEOS` | **10.0** — well below 14.3 |
| root escalation | spawns **`/usr/bin/login`** — rootful, and present in the rootfs |
| Swift linkage | 7 hard (`libswiftCore`, `CoreGraphics`, `Dispatch`, `Foundation`, `ObjectiveC`, `UIKit`, `libswiftos`), 6 weak |
| those 7 in `org.swift.libswift` | **all present** |
| rpath | already includes `/usr/lib/libswift/stable` |
| declared deps | `firmware (>= 10.0)`, `ws.hbang.common (>= 1.16)`, `bash`, `coreutils`, `ncurses`, `system-cmds` — all satisfied |
| `ws.hbang.common` (Cephei) | 1.16, `LC_VERSION_MIN_IPHONEOS 7.0`; declares `mobilesubstrate`, which libhooker `Provides (= 99.4)`, and links `CydiaSubstrate`, which libhooker ships at `/Library/Frameworks/CydiaSubstrate.framework/CydiaSubstrate` |

So the terminal is solved: NewTerm 2.5 + Cephei, both in the fetch list.

### Open

1. **`libsyringe-14` is built for iOS 14.4.** Its `LC_BUILD_VERSION` says
   `minos 14.4`; we run 14.3. The dispatcher (`libsyringe`) picks it when
   `kCFCoreFoundationVersionNumber >= 1664.0`, which 14.3 satisfies — so it
   *will* be chosen. Since neither the kernel nor dyld rejects a high `minos`,
   it should load; whether it *works* is untested. If injection fails, this is
   the first suspect, and the fallback is `libsyringe-13` (minos 13.7), which
   the same dispatcher picks below the threshold.

2. **`jailbreakd` runs without having patched the kernel.** On a real device
   Taurine exploits the kernel first. Here the kernel is ours and already
   permissive, so `jailbreakd` starts in a configuration its authors never
   tested. It is off the critical path (libhooker carries its own injector) and
   gets `RunAtLoad` without `KeepAlive`, so a crash is logged once, not looped.

3. **Sileo's `giveMeRoot` is `minos 15.2`.** Sileo calls it to escalate for
   `dpkg`. Same reasoning as above — nothing rejects it — but it is the piece
   most likely to misbehave, and if it does, Sileo will install nothing.

4. **Nothing here has run on hardware.** The device is in repair. Every line
   above is static analysis of the packages plus reading the kernel and dyld
   sources.

5. **This is all downstream of the boot blocker.** The GUI does not draw
   because `containermanagerd` never gets a persona. No jailbreak code runs
   before that is fixed.

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
