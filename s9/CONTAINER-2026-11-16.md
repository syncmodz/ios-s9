# The container blocker — what the reference ports actually say

**Session 2026-11-16.** Study of the 15 reference ports in `refs/`, aimed at the one wall:
`containermanagerd` → no MCM container answers → `BSSystemSharedResourcesDirectory()` nil →
backboardd aborts in `-[NSURL initFileURLWithPath:]`.

Everything below was checked against a file, a binary or a boot log. Where something is
inference, it says so.

---

## 0. The logs this project has been diagnosing from are the wrong ones

`README.md` says *"`MKB_INIT` does complete and the personas are created (verified in all five
persistent logs)."* Those five logs are `s9port/logs/live-ios1*.txt` and
`s9port/logs/evidence-20261029/userdata-sweetlog-20261029.txt`. Their first line is:

```
Darwin Kernel Version 22.6.0 ... runner:xnu-8796.141.3
```

That is **iOS 16**, not the 14.3 port. The real 14.3 logs (Darwin 20.2.0,
`xnu7195-local`) are in `~/s9work/logs/log13.txt` … `log33.txt` — `log33.txt`
(2026-11-12 14:04) is the latest. Every conclusion drawn from the iOS 16 logs about 14.3
personas is about a different kernel **and a different rootfs** (20H392 vs 18C66).

This matters because the two disagree, and the 14.3 one is much worse (see §3).

---

## 1. Bergamota (iOS 18, SDM845) — read, and rejected

Bergamota is the closest analogue (same SoC family) and it spent 15 kernel patches on exactly
this failure. Its `NOTES.md` is the single most useful document in `refs/` for this problem,
because it documents a chain of *wrong* hypotheses before the right one.

The sequence, in its own order:

| patch | hypothesis | outcome |
| --- | --- | --- |
| **0147** | containermanagerd and usermanagerd race; delay MCM's first exec 6 s | **did not help.** MCM still failed 17× in 20 ms; launchd logged *no* failed lookup, so the lookup succeeds and the connection is interrupted |
| **0148–0151** | sample the ports of both daemons | usermanagerd is *idle* in its runloop after setup, with receive rights at `seqno 0`. containermanagerd is busy (`seqno 623→1001`). "idle-but-unreachable — endpoints not delivering" |
| **0152** | launchd's kevent/MACHPORT watches | watches are fine; launchd deletes them when it launches the job |
| **0153** | `posix_spawn` returning EINVAL | real bug, but for SpringBoard/xpcproxy, not this |
| **0154** | MCM's sqlite DB fails on HFS EINVAL | real, but downstream |
| **0156** | **`mach_port_is_connection_for_service` returns `KERN_NOT_SUPPORTED`** | **this was it** for iOS 18 |

Bergamota 0156's mechanism: libxpc calls `mach_port_is_connection_for_service` for every peer
check-in and accepts only `KERN_SUCCESS`/`KERN_INVALID_CAPABILITY`. xnu-8796 returns
`KERN_NOT_SUPPORTED` when no Sandbox message-filter callbacks are registered — which is always,
without a Sandbox kext. So **every** XPC listener rejected **every** client, and the clients saw
`XPC_INTERRUPTED_REPLY` (container error 52).

**Why that does not apply here.** Our tree has the identical early return
(`osfmk/ipc/mach_port.c:3070`, checked in `work/xnu`), but 14.3's userland never calls it:

- `strings -a` over the 18C66 dyld shared cache finds **zero** occurrences of
  `mach_port_is_connection_for_service` or any `connection_for_service`. It is an iOS 18 API.
- The one place 14.3's libxpc mentions a bad check-in is `Bogus check-in attempt. Ignoring.`
  (`libxpc.dylib` @ `0x1c8b543d5`); the code that reaches it
  (`0x1c8b341e0`…`0x1c8b3424c`) validates an audit-token-derived policy id, not a service-port
  check-in. Not the same gate.

So `0147` and `0156` are both dead ends for us. That is worth stating plainly, because 0147 is
the patch we would have copied first.

**What Bergamota does teach us, and we should keep:**

- Its **diagnosis method** is better than ours. `0148`–`0151` (per-process receive-right dumps
  with `seqno`/`knote` per port) distinguish "the client never sent" from "the listener never
  dequeued" from "the endpoint was never handed out". Our `sweet_sample.c` prints backtraces
  only (91 lines) and has no port dump at all. In our 14.3 log `sweetsample=-` is set, so it
  never even ran.
- The **60-retry loop** we crash in is Bergamota's `MCMUserIdentityCache _lock_resync`. Its
  reading of the same code is right: it enumerates through `listAllPersonaWithAttributes`
  (`UserManagement.framework` → `com.apple.mobile.usermanagerd.synchronous.xpc`) and gives up
  when the list is empty — not when a persona is missing from the kernel.

---

## 2. ios-topaz and ios-sweet — same failure, and they name the file

`ios-topaz/NOTES.md:82` (and `ios-sweet/NOTES.md:88`, same text) is the 16.7.16 Android-phone
port hitting our exact wall, with the stack trace:

```
+[NSURL fileURLWithPath:]
  <- -[BSSystemContainerForCurrentProcessPathProvider libraryPath]
  <- cachesPath <- backboardd main+0x1d858 (StartWindowServer, shader cache path)
```

and the decisive log line, which we have never looked for:

> usermanagerd does create kernel personas 99/199/1000, but **fails to save
> `/private/var/keybags/usersession.kb` and `persona.kb`**:
> `Failed to atomically swap writing file: Operation not supported (45)`
> (HFS has no `RENAME_SWAP`).

topaz's own next step was: *fix or emulate RENAME_SWAP on HFS.*

**We already did that.** `xnu/patches-7195/bsd_vfs_kpi_vfs_c.diff` turns `ENOTSUP` +
`VFS_RENAME_SWAP` into `VNOP_EXCHANGE`. So that specific link is closed — but the comment in
that patch quotes the *iOS 16* error string, and there is no evidence in the 14.3 logs that the
swap path is what is failing now (see §3).

topaz also records that `swatchd`/`containermanagerd` needs `/private/var/containers`, which
"does NOT exist in the root image (on iOS it lives on the data volume)". Our `sweet_fixup`
already creates that whole tree (`bsd_vfs_vfs_syscalls_c.diff`), and the 14.3 boot log confirms
it (`SWEET fixup mkdir /private/var/containers/... err=17`, i.e. already present).

---

## 3. What the real 14.3 boot actually shows — and it is not what the README says

From the instrumented boot in `~/s9work/logs/log33.txt` (Darwin 20.2.0, `xnu7195-local`,
2026-11-12). The `PGET`/`PFIND`/`PRESP`/`PALLOC` probes from
`xnu/patches-7195/bsd_kern_sys_persona_c.diff` ran; 45 lines.

### 3.1 containermanagerd never touches the kernel persona API

Every process that reads a persona prints `PGET`. In this boot:

```
PGET UserEventAgent, wifid, identityservices, rapportd, backboardd, powerd,
     amsaccountsd, wcd, navd, cloudpaird
```

**`containermanagerd` is not in that list, and prints no `PRESP` and no `PFIND` either.** It
does not call `kpersona_info` at all before it dies — which matches the README's own note that
`_kpersona_info` is its only kernel import. Whatever it is missing, it is asking for **over
XPC**, and the answer is not coming back.

**Consequence: no change to the kernel persona code can fix this.** The extensive persona work in
`bsd_kern_kern_persona_c.diff` / `bsd_kern_sys_persona_c.diff` (v1/v2 layouts, `pna_uid`,
`persona_get_uid`) is not on the critical path. It is correct work — `PRESP` shows the v1/v2
handling working and `pna_uid` reporting real uids instead of −101 — but it is not the wall.

### 3.2 The crash is exactly where the README says, and the retry budget is 60

`SIGTRAP containermanager pc=0x102cb6580`. `containermanagerd`'s `__TEXT` is at `0x100000000`
with slide `0x2cb0000`, so that is `+0x6580`, and in `cm.dis`:

```
0x100006574  adr  x0, "Could not obtain persona attributes after exhausting retries."
0x10000657c  bl   __os_crash
0x100006580  brk  #0x1
```

The loop that leads there (`_lock_resync`, `0x100006120`…`0x100006294`):

```
0x1000061f0  cmp  x26, #0x3c        ; 60 attempts
0x1000061f4  b.ne 0x100006280       ; retry
0x1000061f8  b    0x1000064bc       ; exhaust -> os_crash
0x10000628c  mov  w0, #0x1
0x100006290  bl   _sleep            ; 1 s between attempts
```

Each attempt calls `-[UMUserManager sharedManager]` then
`listAllPersonaWithAttributes`, and succeeds only if the returned collection is non-nil and
has `count != 0` (`0x1000061c0`–`0x1000061e4`). So the failure is **an empty or nil persona
list from UserManagement**, after 60 seconds of trying.

### 3.3 The persona set is short by two, and the user session never loaded

iOS 16 boot (`evidence-20261029`, the working one) — five personas:

```
99 type 4 uid 4294967295    199 type 6 uid 4294967295    1000 type 5 uid 501
200 type 6 uid 501          100 type 4 uid 501
```

14.3 boot (`log33.txt`) — four, and **100 and 200 are absent**:

```
1000 type 5 uid 1000        99 type 4 uid 99        199 type 6 uid 199
501  type 2 uid 501   (created by launchd, not keybagd)
```

And immediately before them:

```
MKB_INIT: Could not open /private/var//keybags/usersession.kb: No such file or directory
MKB_INIT: LoadAllUserSessions: diskLoadUserSessions failed
```

Then keybagd goes straight to allocating personas and finishes
(`MKB_INIT: setup kernel personas` / `DONE MKB_INIT`). It never creates a user session, and the
per-user personas 100/200 that iOS 16 creates *after* loading a session never appear.

**Hypothesis (not proven):** `listAllPersonaWithAttributes` in 14.3 is served from the user
session table. With `diskLoadUserSessions` failed, the table is empty, the list comes back
empty, and MCM's retry loop exhausts. The missing 100/200 is the visible symptom of the same
thing.

**How to prove it in one boot, without a device change to the flow:** the keybagd daemon
(`pid 38`) logs `pthread_register` and then a single `AKS sel 0` and nothing else for the rest
of the boot. That is the same "idle but unreachable" signature Bergamota measured. Add
Bergamota's port probe (`0149`-style: per receive-right `seqno`/`knote`) for
`pid 38` and `pid 61`; if keybagd's listener shows `seqno 0` while MCM's client port shows
queued messages, it is the same endpoint problem, and if the listener shows `seqno > 0` with an
empty reply it is the session table.

### 3.4 Two more things in log33 that are not in any doc

- `PGET backboardd persona=present id=199` — backboardd *has* persona 199 and still aborts, so
  its own persona is not the problem; the nil is the container path.
- `PGET ... id=199` for nearly every daemon, i.e. they are all in `SYSTEM_PROXY` (type 6). In
  Bergamota's iOS 18 boot the same `200 = SYSTEM_PROXY` fallback was recorded as *"the personal
  persona of user 501 does not exist, so launchd falls back to the proxy persona"* — the same
  reading applies here, and it is consistent with §3.3.

---

## 4. What we changed in our own code because of this

Nothing yet — this session is study. The patches that follow from it:

1. **Stop porting persona fixes.** The kernel persona path is not on the critical path (§3.1).
   Do not add Bergamota `0147` (delay) or `0156` (it is an iOS 18 API we do not have).
2. **Fix `diskLoadUserSessions`.** The one concrete divergence from a working boot. Options,
   cheapest first: (a) create a valid `/private/var/keybags/usersession.kb` in the root image;
   (b) make keybagd's fallback path run — 14.3's keybagd *has* the strings
   (`FOUND NO SYNC BUBBLE UIDS`, `SAVING SESSION TODISK`, `FOUND NO VOLUMETYPE, setting up volume
   type`) but never prints them, so the code exists and bails early; (c) instrument
   `LoadAllUserSessions` to say which check fails.
3. **Add a port probe** to `bsd/dev/sweet_sample.c` (Bergamota `0149`/`0150`), so the next boot
   answers "did keybagd's listener ever receive the request" instead of us guessing.
4. **Do not trust the iOS 16 logs for 14.3.** `s9port/logs/` is xnu-8796; the 14.3 logs are in
   `~/s9work/logs/`. This one cost several wrong conclusions.

---

## 5. Display — what ios-topaz adds (for later)

Not this session's task, but recorded because it is the same document
(`ios-topaz/NOTES.md:117`): on that port backboardd died **before** any display lookup —
`IODeviceTree:/arm-io/pearl-sep` → none, `ApplePearlSEPDriver` → none, AppleKeyStore OK, IOHID
notifications, then the exception. **No `IOMobileFramebuffer` lookup ever happened.** So
"the GUI does not draw" there was never a panel problem first; it was this container problem.
Our log33 shows exactly the same ordering (backboardd logs the same three IOKit probes at
`log33.txt:657`–`676`, then `NSInvalidArgumentException` at `:683`).

topaz's conclusion is worth carrying over verbatim: the display wall is *after* the container
wall, and there are two ways through it — `CA::WindowServer::AppleCLCDDisplay` (our
IOMobileFramebuffer path) or a `CA::WindowServer::VirtualDisplay` rendered by CA's CPU path.
We already have the former implemented. Do not reorder this.
