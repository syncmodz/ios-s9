# Prompt to resume this project in a new session

Copy everything between the lines into a fresh session.

---

## The project

I am porting **iOS 14.3 (18C66)** to a **Samsung Galaxy S9 (SM-G9600, `starqltechn`,
Snapdragon 845 / SDM845)**. Apple's open-source **xnu-7195** runs bare-metal on the
phone, and on top of it the unmodified iPhone XR iOS 14.3 userland: launchd, the
daemons, backboardd, SpringBoard.

Repository: **https://github.com/syncmodz/ios-s9** (branch `main`). Local clone:
`~/Projetos/iOS port para o S9/ios-s9`.

Read these first, in this order:

1. `README.md` — what the project is, where it stands, the open blocker.
2. `s9/NOTES.md` — the engineering log (Portuguese). Every boot, crash and decision.
3. `s9/jailbreak/README.md` — the jailbreak layer, audited against the real binaries.
4. `refs/REFS.md` — the 15 reference ports and what each one teaches.

## Where it stands

The kernel reaches userspace: launchd plus ~47 daemons, `Early boot complete` in
about 3 s, SpringBoard alive. **The GUI does not draw.**

The open blocker, in order:

```
containermanagerd  -> 60 XPC queries to usermanagerd, gives up, SIGTRAP at +0x6580
                   -> no MCM container answers
                   -> BSSystemSharedResourcesDirectory() = nil
                   -> backboardd aborts in -[NSURL initFileURLWithPath:]
                   -> SpringBoard runs, but draws nothing
```

In 14.3 `usermanagerd` *is* keybagd — there is no separate binary. `MKB_INIT`
completes and personas are created (verified in all five persistent logs), so the
failure is downstream of that. The next boot is instrumented: `PGET`/`PFIND` print
per process, so `containermanagerd`'s own view of its persona is the first thing
the log will show.

**The device is in repair.** Nothing has run on hardware recently; everything in
the jailbreak layer is static analysis.

## What I want to do next

**Study the reference code we already have but have not used.** We have 3.5 GB of
ports in `refs/` (see `refs/REFS.md`), and only a fraction has been read. The goal
is to mine them for things that improve *this* port — not to read them cover to
cover.

Concretely, the areas where they are most likely to help, in priority order:

1. **The container/persona blocker.** This is the wall. Look for how other ports
   got past `containermanagerd` / `BSSystemSharedResourcesDirectory`. Check
   `ios-flame`, `ios-topaz`, `ios-sailfish`, `Bergamota` and `HakiOS` for
   container, persona, keybag or `MCM` handling. `Bergamota` is iOS 18 on SDM845 —
   the same SoC family — so its container path is the closest analogue.
2. **The display path.** The kernel IOSurface + IOMobileFramebuffer services exist
   but the panel is not driven. `ios-topaz`'s `NOTES.md` has the exact console
   lines that prove backboardd drew; `ios-sailfish` has the ordered bring-up.
3. **GPU.** Currently CPU-rendered. `ios-flame` has Metal working on an Adreno;
   `Chlorophyll` and `metal2vulkan` are the two routes. This is the hardest item
   and should not be started before the GUI draws at all.
4. **The jailbreak layer.** `s9/jailbreak/README.md` lists what is verified and
   what is open. The main unknown is `libsyringe-14` being built for 14.4.

## How I want you to work

- **Verify, do not assume.** Every claim in this repo was checked against the
  actual binaries, the actual rootfs, or the actual source. When you say something
  works, say how you know. When you have not checked, say that too.
- **Read the real files.** `s9hfs.py` in `s9/tools/` reads the HFS+ root image
  offline; `otool`, `nm`, `codesign` and `ar` are how the debs were audited. Do not
  guess at a `minos`, a dependency or a path — look.
- **The rootfs already has the Taurine bootstrap.** `/Library/dpkg/status` is
  byte-identical to the tarball (58 packages). Do not install a second one.
- **Taurine is rootful.** Proven from `bootstrap.swift`: `tar -xkf ... -C /`.
  Rootless (`/var/jb`) is iOS 15+ and does not apply here.
- **Do not create a website.** The `site/` directory is the original ios-sweet
  docs and is not ours to extend. When I say "update the page", I mean
  `README.md` on GitHub.
- **Commit and push when a piece is done**, with a message that says what was
  verified and how. `gh` is authenticated; no device code needed.
- **Ask before anything destructive on the device.** Flashing, wiping, or
  repartitioning needs my explicit go-ahead each time.

## Useful facts that took a while to establish

- `task_for_pid` as root passes with no entitlements:
  `task_for_pid_posix_check` in `bsd/vm/vm_unix.c` returns TRUE for
  `kauth_cred_issuser` before any other test. No MAC policy in this tree
  implements `mpo_proc_check_get_task`, so the MACF gate returns allow. This is
  why libhooker's injector should work.
- Neither the kernel nor dyld rejects a high `minos` — checked in `mach_loader.c`
  and dyld-852.2. So a 14.4-built dylib loads on 14.3; whether it *works* is
  another matter.
- The `ppnum_t` truncation in `pmap_sync_page_attributes_phys` affected 2018 MB of
  this device's RAM. Fixed as `ptoa(pp)`. Found in `ios-flame`'s `METAL-NEXT.md`.
- A kernel panic never reached the persistent log until `consdebug_putc` and its
  unbuffered twin were made to append to the ring. That is what makes a failed
  boot inform instead of requiring a photo.
- NewTerm 3 cannot work here (needs `libswiftUniformTypeIdentifiers`, which is
  14.5+, and is the rootless build). **NewTerm 2.5** is the right one.
- Sileo needs `org.swift.libswift`; 14.3 ships only `libswiftDemangle` and
  `libswiftXCTest`.

## First thing to do

Read `README.md`, `s9/NOTES.md` and `refs/REFS.md`, then tell me what you found in
the reference ports about the container/persona blocker — that is the wall, and
anything that gets us past it is worth more than everything else combined.

---
