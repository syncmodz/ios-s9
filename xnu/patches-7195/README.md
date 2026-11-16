# XNU 7195 — the iOS 14.3 kernel tree

This directory is the kernel side of the port: Apple's **xnu-7195** (the iOS 14.3
generation) with the changes that make it boot on the Galaxy S9.

It is not a patch series applied in order like `../patches/` (that one is the
iOS 16 / xnu-8796 tree). This is a **snapshot diff**: every file that differs
from Apple's release, as one patch per file, plus a tarball of the files that
did not exist upstream.

## How to rebuild the tree

```sh
# 1. Apple's release, unmodified
curl -LO https://opensource.apple.com/tarballs/xnu/xnu-7195.141.2.tar.gz
tar xzf xnu-7195.141.2.tar.gz && cd xnu-7195.141.2

# 2. the files that are new (HFS+, BearSSL, IOHID, the sweet drivers, ...)
tar xzf /path/to/patches-7195/new-files.tar.gz

# 3. the files that changed
for p in /path/to/patches-7195/*.diff; do patch -p1 < "$p"; done
```

All 80 patches apply cleanly against the pristine release — verified with
`patch -p1 --dry-run` over a fresh copy.

## What is in here

| Group | Files |
| --- | --- |
| **Build system** | `config/MASTER`, `config/MASTER.arm64.sweet`, `makedefs/MakeInc.*`, `SETUP/config/mkmakefile.c`, the per-directory `Makefile`s and `conf/files` |
| **Machine / boot** | `osfmk/arm64/start.s`, `arm_vm_init.c`, `cpu.c`, `machine_routines.c`, `osfmk/arm/arm_init.c`, `pmap.c`, `rtclock.c`, `pexpert/arm/pe_init.c` |
| **Console** | `osfmk/console/serial_console.c`, `osfmk/kern/printf.c` (the panic path now reaches the persistent log) |
| **BSD core** | `bsd_init.c`, `kern_exec.c`, `kern_exit.c`, `kern_core.c`, `kern_cs.c`, `kern_mib.c`, `sys_generic.c`, `syscalls.master` |
| **Personas** | `kern_persona.c`, `sys_persona.c`, `bsd/sys/persona.h` — the probes that print `PGET`/`PFIND` |
| **VFS** | `vfs_syscalls.c` (the rootfs fixup), `vfs_lookup.c`, `vfs_subr.c`, `vfs_conf.c`, `kpi_vfs.c`, `bindfs` |
| **Crypto** | `corecrypto_aes.c`, `corecrypto_sha2.c`, `corecrypto_md5.c`, `prng_random.c`, `cckprng.h` |
| **Memory** | `vm_kern.h`, `vm_unix.c`, `kalloc.h`, `zalloc.c`, `task.c` (the jetsam floor) |
| **IOKit** | `KernelConfigTables.cpp`, `IOPlatformExpert.cpp`, `IOStartIOKit.cpp`, `IOUserServer.cpp`, `IOKitKeys.h`, `IOLib.h` |
| **New files** | `bsd/dev/sweet_*.c` (UFS, USB, keys, touch, SPMI, RPMh, watchdog, sampler, BCB), `bsd/sweet_hfs/` (Apple's HFS+), `bsd/pthread/sweet_libpthread/`, `libkern/crypto/sweet_bearssl/`, `iokit/Kernel/arm/hid/`, `libkern/amfi/`, `osfmk/arm64/sweet_early.c` + `sweet_logo.h` |

## The two fixes worth knowing about

**`osfmk/arm/pmap.c` — `pmap_sync_page_attributes_phys`.** It did
`flush_dcache((vm_offset_t)(pp << PAGE_SHIFT), ...)`. `ppnum_t` is 32 bits, so
the shift truncates: any physical address at or above 4 GB flushes the wrong
line. This device's DRAM runs to `0x17e1c0000`, so **2018 MB of RAM** was
affected. Now `ptoa(pp)`. The same bug is documented in `ios-flame`'s
`METAL-NEXT.md` §4.1, which is where it was found.

**`osfmk/kern/printf.c` — the panic path.** `consdebug_putc` and
`consdebug_putc_unbuffered` bypass the console's log ring: the buffered one goes
to `cnputc()`, the unbuffered one straight to `_cnputc`, both past the ring that
`_cnputs` feeds. A kernel panic therefore existed only on the screen, and the
backtrace died with the reset. Both now also append to the lock-free ring in
`bsd/dev/sweet_ufs.c`.

## Status

The tree builds clean (0 errors) and produces a kernel that reaches userspace:
launchd, ~47 daemons, `Early boot complete`, SpringBoard alive but not drawing.
The open blocker is the container/persona chain — see `../../s9/NOTES.md` and
the audit reports in the project's working directory.

The build ends with a non-zero status on `check_all_exports` (symbol-set
generation). That is expected and harmless: `mach.development.sweet` is already
linked by then. The same failure is present in the iOS 16 tree.
