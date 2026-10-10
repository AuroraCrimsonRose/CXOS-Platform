# CXK ABI v2 — Syscall & Capability Contract

**Status:** LIVE CONTRACT (v2). The register convention, capability model, handle/IPC
semantics, and error codes are stable and shipped. Both the `.xoex` executive and the
X Native / X Runtime / X Hybrid toolchain compile against this. Source of truth is **`abi/cxk_abi.h`**; this
document explains it. If the two disagree, the header wins and this document is the bug.

**What changed from v1, and why this is v2.** The v1 document declared the whole number
space frozen, including ranges for calls that were only ever *specified* — `map`/`unmap`/
`sbrk` at `0x20–0x22` and `block_read`/`block_write` at `0x40–0x41`. Those calls were never
implemented, and input (`0x20–0x21`) and the framebuffer (`0x40`) were subsequently
allocated over them. Networking, listed in v1 as reserved and unimplemented, also shipped
at `0x50`.

v2 resolves that honestly rather than pretending it did not happen:

- **Every number that has ever shipped keeps its value.** That was the promise worth
  keeping, and it is intact — no implemented call has been renumbered.
- **Paper allocations for unimplemented calls were not load-bearing** and have been moved
  (§7.8, §7.9) to ranges that are actually free.
- The rule going forward is narrower and therefore keepable: **a number is frozen once it
  ships**, not when it is written down. Unimplemented reservations are advisory.

Source basis: `abi/cxk_abi.h`, `cpu/usermode.c`, `cpu/caps.h`, `cpu/exec.c`,
`lib/format/cxex_verify.c`.

---

## 1. Model (brokered exokernel — "B")

- The **kernel** is pure mechanism and owns the ring 0/3 wall. It exposes a small set of
  primitive syscalls and the IPC machinery. It has **no knowledge of "the executive"** — it
  only knows which process was handed which capabilities at launch.
- An **`.xoex` executive** is the only ring-3 holder of privileged capabilities. It is a
  *broker*: it owns an IPC endpoint and services requests from its apps using its own caps.
- **Apps** are ring-3 processes that are **capability-less**. Their only useful syscall is
  `ipc_call` to their broker endpoint, plus the always-available lifecycle calls. An app that
  tries a privileged primitive directly gets `E_PERM`.
- **Authority comes from `caps_for(type, trusted)` at the `cxex_exec` handoff**, never from
  the signature itself. The signature establishes identity + integrity; this function maps
  identity → capability set.

### End goal this v1 is shaped toward (not implemented in v1)
Multiple side-loaded executives — a CXOS-native `.xoex`, a Linux-subsystem `.xoex`, a
Windows-subsystem `.xoex` — each holding its **own** capability set and owning its **own**
endpoint, brokering its **own** apps. Because endpoints are objects and caps are
per-executive, "launch a second executive" is configuration, not redesign.

### What this ABI still defers (all purely additive)
- **Shared-memory grants.** Bulk data moves as bounded, ≤1-page, **kernel-copied** reply
  payloads. Shared memory is a v-next primitive; the `ipc_call` shape already leaves room.
- **Multiple concurrent executives** (one is launched today).
- **Concurrent in-flight IPC** (an endpoint serves one call at a time).
- **Filesystem access from ring 3** — the largest gap. See §7.10.

No longer deferred: **app self-service** is now a spectrum rather than a rule. v1 said apps
are maximally thin and everything privileged goes through the broker; capability attenuation
at `spawn` (§5) means an executive can instead hand a child exactly the authority it needs.
The brokered path remains the default and the thin-app model still works unchanged — a child
given `caps=0` behaves exactly as v1 described.

---

## 2. Calling convention (proven; do not change)

| Register | Role |
|----------|------|
| `eax` | syscall number (in) / signed result (out) |
| `ebx` | arg1 |
| `ecx` | arg2 |
| `edx` | arg3 |
| `esi` | arg4 |
| `edi` | arg5 |

- Invoke with `int 0x80` (IDT gate DPL=3 so ring 3 may call it).
- **Result** in `eax`: `>= 0` success (value is call-specific), `< 0` is a negative error
  code (§4).
- All general-purpose registers **except `eax`** are preserved across the call (the stub
  `pushad`/`popad`s and writes only the result back into saved `eax`).
- Segments: user `CS = 0x1B`, `SS/DS/ES/FS/GS = 0x23`; the stub switches to kernel `0x10`
  on entry and restores on `iretd`.
- **64-bit values** (LBAs, file sizes/offsets) are **never** split across registers. They
  are passed inside an argument struct in user memory; the syscall takes a *pointer* to that
  struct (validated per §3). This keeps the register convention uniform.

---

## 3. Pointer & fault rules (security invariants)

- The kernel **never dereferences a ring-3 pointer** without validating it, against the
  **calling process's** mapped user region, length-bounded. Failure → `E_FAULT`.
- **Ask for the access you intend.** There is no single check: `user_ptr_readable(ptr, len)`
  requires present + ring-3-accessible, and `user_ptr_writable(ptr, len)` additionally
  requires `PAGE_RW`. The combined `user_ptr_ok` is deliberately gone rather than kept as an
  alias, so a new call site has to say which it means and cannot default to the weaker check
  by forgetting (security review §4).
- Maximum single user transfer is bounded (one page, `0x1000`, for messages; primitives
  state their own caps).
- **Copy with `user_copy_out` / `user_copy_in`; never validate and then dereference.** A
  validation is only true at the instant it runs. Any call that validates, blocks, and then
  copies is trusting a snapshot nothing has refreshed — which is what both IPC blocking paths
  did (security review §6). These copy primitives re-validate at the copy *and* perform it
  inside a fault-recoverable region, so a mapping that has gone away yields `E_FAULT` instead
  of a stale-probe write or a ring-0 fault. On failure the destination is **partially
  written** and must be treated as undefined.
- **`CR0.WP` is set** (in `kernel.asm`, with `PG`, so it holds for the whole life of the paged
  kernel). The CPU therefore enforces the read-only bit against ring 0 as well: a kernel write
  to a read-only page faults like any other write violation, instead of silently succeeding
  and leaving the page tables claiming it could not have happened.
- **Both layers are still load-bearing, and they are not the same check.** `WP` enforces the
  **write** bit only. It says nothing about `PAGE_USER`, and nothing stops ring 0 writing to a
  kernel address — so a copy-out aimed at a kernel address, or at a page that is writable but
  not ring-3-accessible, passes the hardware and is caught only by the range check. A fault
  also stops the copy *where it faults*: for a buffer spanning several pages, writable at the
  start and read-only later, recovery alone leaves the earlier pages modified, while the check
  refuses the whole transfer before anything moves. Conversely no check can cover a page that
  stops being mapped after the check and before the instruction that uses it, which is what
  recovery is for.
- Fault recovery is scoped by **where the fault happened** — the faulting EIP inside
  `usercopy_start`…`usercopy_end` (`cpu/usermode.asm`), checked in `cpu/int/idt.c` — and not
  by an armed flag, so there is nothing that can be left switched on. A kernel page fault
  anywhere else still panics.
- **A bounded scan is not a bounded read.** Scan for a length, then read exactly that many
  bytes; never hand the raw pointer to something that walks to its own NUL, or a string
  filling a mapped page with no terminator reads past the mapping. `SYS_CONSOLE_WRITE` and
  `FB_OP_DRAW_TEXT` (via `fb_draw_string_n`) both follow this. Any future call taking a user
  string must too.
- A CPU fault taken while in ring 3 routes to the user-fault hook, which **terminates the
  faulting process** and returns to the scheduler. A buggy app/executive never takes down
  the kernel.

---

## 4. Error codes (negative, errno-style)

| Value | Name | Meaning |
|------:|------|---------|
| 0  | `E_OK`    | success (or a non-negative call-specific value) |
| -1 | `E_PERM`  | capability denied |
| -2 | `E_INVAL` | bad argument |
| -3 | `E_FAULT` | bad/unmapped user pointer |
| -4 | `E_NOENT` | no such object |
| -5 | `E_NOMEM` | out of memory |
| -6 | `E_BADF`  | bad handle |
| -7 | `E_AGAIN` | would block / no message ready |
| -8 | `E_RANGE` | message too large / buffer too small |
| -9 | `E_NOSYS` | unknown syscall number |

---

## 5. Capability model (per-process bitmask)

Capabilities live in the kernel's process record — **never in user memory**, so ring 3
cannot forge them. The dispatcher gate is one line: `if (!(cur->caps & GRANT_X)) return E_PERM;`

| Bit | Name | Gates | State |
|----:|------|-------|-------|
| 0x0001 | `GRANT_CONSOLE`     | `console_write` | live |
| 0x0002 | `GRANT_MEM`         | `map`, `unmap`, `sbrk` | **gates nothing yet** — those calls are unimplemented (§7.8) |
| 0x0004 | `GRANT_DISK`        | `block_read`, `block_write` | **gates nothing yet** — those calls are unimplemented (§7.9) |
| 0x0008 | `GRANT_NET`         | `net_op` | live |
| 0x0010 | `GRANT_SPAWN`       | `spawn` | live |
| 0x0020 | `GRANT_POWER`       | `power` | live |
| 0x0040 | `GRANT_ENDPOINT`    | `ep_create` (may own an IPC endpoint, i.e. may be a broker) | live |
| 0x0080 | `GRANT_IOPORT`      | raw port I/O / driver tier | reserved, unimplemented |
| 0x0100 | `GRANT_FRAMEBUFFER` | `fb_op` | live |

Always-available calls (lifecycle, `input_read`, `mouse_read`, `ipc_call`, `handle_close`)
require **no** capability. Input is deliberately unprivileged: a process reading the keyboard
or the pointer it is already being shown is not an escalation.

> **Honest note on `GRANT_MEM` and `GRANT_DISK`.** Both bits are defined and both are included in
> `GRANT_OS_BASELINE`, so the executive is granted them — but no syscall consults them, because
> `map`/`unmap`/`sbrk`/`block_read`/`block_write` do not exist. They are granted authority over
> nothing. This is harmless but it is not nothing: a reader of `caps.h` reasonably concludes
> the executive can touch raw disk, and it cannot. Keep the bits (they are the right
> allocation) and treat §7.8/§7.9 as the work that makes them real.

### `caps_for(type, trusted)` — the policy layer
```
caps_for(type, trusted):
    if (!trusted)                   return 0;                     // verified but untrusted: nothing
    if (type == CXEX_TYPE_OS)       return GRANT_OS_BASELINE;     // the broker executive
    if (type == CXEX_TYPE_SYSTEM)   return GRANT_SYSTEM_BASELINE; // OS-owned, not the OS
    if (type == CXEX_TYPE_USER)     return 0;                     // apps are capability-less
    return 0;

GRANT_OS_BASELINE     = GRANT_CONSOLE | GRANT_MEM | GRANT_DISK | GRANT_NET
                      | GRANT_SPAWN | GRANT_POWER | GRANT_ENDPOINT
                      | GRANT_FRAMEBUFFER                          /* = 0x017F */
GRANT_SYSTEM_BASELINE = GRANT_CONSOLE | GRANT_DISK                 /* = 0x0005 */
```
- `CXEX_TYPE_OS` = `0x4F45` `'OE'`, `CXEX_TYPE_SYSTEM` = `0x5345` `'SE'`,
  `CXEX_TYPE_USER` = `0x5545` `'UE'` (from the CXEX type-code family). The old
  `0x4345 'CE'` (`.xcex`) is retired: it named an executable for how it was
  built, and every executable is compiled.
- **The system tier is narrower than the executive's on purpose.** A `.xsex` is
  OS-owned but is not the OS, so it starts with enough to say something and to
  read its own files, and asks for anything more by being started with it.
- `GRANT_FRAMEBUFFER` is in the baseline **for now**. When a display-server tier exists, the
  compositor should hold it and ordinary apps should draw through that server, not directly.
- `caps.h` and `abi/cxk_abi.h` each carry a copy of `GRANT_OS_BASELINE` (userspace spawners need
  it to request caps). The header guards and static-checks them against each other so the two
  cannot silently drift.

### Capability attenuation at `spawn` (implemented — was deferred in v1)
v1 specified that the kernel forces a spawned app's caps to `0`. That has been superseded:
`spawn_args` now carries a `caps` field and the kernel **attenuates** it against the
spawner's own set:

```c
uint32_t granted = a.caps & thread_current_caps();   /* cpu/spawn.c */
```

You may pass a subset of your own authority and never amplify. An app-spawner holding `caps=0`
therefore still produces children with `caps=0`, which preserves v1's behaviour as the
degenerate case while allowing an executive to hand a child exactly the authority it needs.

---

## 6. Handle model (per-process object table)

Each process has a fixed-size **handle table** (v1: 16 slots). A handle is a small
non-negative integer index; **handle `< 0` / out of range / empty slot → `E_BADF`**.

```c
#define CXK_MAX_HANDLES 16
enum handle_type { HANDLE_NONE = 0, HANDLE_ENDPOINT = 1 };  /* v1: endpoints only */
#define HRIGHT_SEND  0x1
#define HRIGHT_RECV  0x2
struct cap_handle { uint8_t type; uint8_t rights; uint16_t _pad; void *object; };
```

- **v1 stores exactly one object type: `HANDLE_ENDPOINT`.** The table mechanism is the part
  that scales to the end goal (file handles, shm regions, more endpoints later); limiting it
  to endpoints is the v1 simplification.
- **Convention: handle `0` in an app is its broker endpoint** (a `HRIGHT_SEND` handle to its
  executive's endpoint), installed by the kernel at `spawn`. Apps don't discover it — they're
  born with it.

---

## 7. Syscall reference (v1)

Number space is partitioned so additions slot in cleanly. Reserved ranges are noted in §9.

### Lifecycle — `0x00–0x0F` (no capability required)

| # | Name | Args (ebx, ecx, …) | Returns |
|---|------|--------------------|---------|
| 0x00 | `exit`   | a1 = exit code | does not return |
| 0x01 | `yield`  | — | 0 (reschedules) |
| 0x02 | `getpid` | — | caller pid |
| 0x03 | `getuid` | — | caller owning uid (0 = SYSTEM) |

### IPC — `0x10–0x1F`

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x10 | `ipc_call`     | a1=endpoint handle, a2=req ptr, a3=req len, a4=reply ptr, a5=reply cap | none | reply length, or `-E_*` |
| 0x11 | `ipc_recv`     | a1=msg buf ptr, a2=buf cap, a3=sender-out ptr | owns a `HRIGHT_RECV` endpoint | request length |
| 0x12 | `ipc_reply`    | a1=data ptr, a2=len | (mid-recv) | 0 |
| 0x13 | `ep_create`    | — | `GRANT_ENDPOINT` | new endpoint handle (`HRIGHT_RECV`) |

### Handles — `0x16–0x1F`

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x16 | `handle_close` | a1=handle | none | 0 |

### Input — `0x20–0x2F` (unprivileged)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x20 | `input_read` | a1=flags (0 = block, 1 = non-blocking) | none | char, 0 if none |
| 0x21 | `mouse_read` | a1=`*mouse_state` | none | 1 if a mouse is present, 0 if not |

```c
struct mouse_state { int32_t x, y; uint32_t buttons; uint32_t seq; };
/* Absolute position, already clamped to the screen by the kernel driver, so userspace
   never sees relative deltas. `buttons`: bit 0 left, 1 right, 2 middle. `seq` increments
   on every state change — compare against the previous read to detect movement without
   diffing coordinates. */
```

### Console — `0x30–0x3F` (privileged)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x30 | `console_write` | a1=buf ptr, a2=len (0 = bounded NUL-scan) | `GRANT_CONSOLE` | bytes written |

### Framebuffer — `0x40–0x4F` (privileged)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x40 | `fb_op` | a1=`*fb_op_args` | `GRANT_FRAMEBUFFER` | op-specific |

```c
struct fb_op_args { uint32_t op, x, y, w, h, color, color2; const char *text; uint32_t *out; };
/* op: 0 INFO (out[0..3] = width,height,bpp,pitch), 1 CLEAR, 2 FILL_RECT, 3 PUT_PIXEL,
   4 DRAW_LINE (w,h carry the END point), 5 DRAW_TEXT.
   Colors cross the ABI as canonical 0x00RRGGBB and are converted to the active mode. */
```

One call per primitive is the current shape and it is the GUI's main cost: a window is
roughly ten syscalls and a cursor seven. A batching op (an op list, or a `BLIT` taking a
user-space pixel buffer) is the intended evolution and needs no ABI break — it is another
`op` value.

### Network — `0x50–0x5F` (privileged)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x50 | `net_op` | a1=`*net_op_args` | `GRANT_NET` | op-specific |

```c
struct net_op_args { uint32_t op; uint32_t ip; const void *data; uint32_t len; uint32_t *out; };
/* op: 0 STATUS, 1 MAC (data = 6-byte buffer), 2 GET_IP (out[0..3] = ip,mask,gw,dns1),
   3 SET_IP (ip = value, len selects field: 0 ip, 1 mask, 2 gw, 3 dns),
   4 PING (ip = destination, len = sequence -> out[0] = rtt), 5 SEND (raw frame),
   6 RECV (raw frame into data/len -> bytes, 0 if none). */
```

Transport is **not** in the kernel: there is ARP, ICMP and IPv4 addressing, and no TCP or
UDP. `SEND`/`RECV` move raw Ethernet frames, so a userspace stack is possible today; an
in-kernel UDP/TCP tier would add ops here or take its own range.

### Process — `0x70–0x7F` (privileged)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x70 | `spawn` | a1=`*spawn_args` | `GRANT_SPAWN` | new pid, or `-E_*` |
| 0x71 | `power` | a1=action, a2=ms (SLEEP only) | `GRANT_POWER` | does not return on reboot/shutdown |

`power` actions: `0` `POWER_REBOOT`, `1` `POWER_SHUTDOWN` (ACPI S5 soft-off), `2` `POWER_SLEEP`
(`a2` = ms; 0 = S1/C1 until a keypress). `SLEEP` **does** return; the other two do not.

```c
struct spawn_args {
    const void *image;   uint32_t image_len;   /* the app's CXEX bytes, in the caller's space */
    const char *name;
    int         broker_endpoint;               /* RECV handle the executive owns; the kernel
                                                  installs a SEND handle to it as the child's
                                                  handle 0. */
    uint32_t    caps;                          /* requested; attenuated by the spawner's own
                                                  set (§5). Pass 0 for a capability-less app. */
    const char *args;    uint32_t args_len;    /* the argument blob (§7.7.1), or NULL/0 */
};
```

#### 7.7.1 Program arguments

A launcher hands over a **flat blob**: the argument strings, one after another, each
NUL-terminated. `args_len` is the whole thing, terminators included. Both `spawn` and
`exec_path` take it, and both refuse a blob that is larger than `USER_ARGS_MAX`, that the
caller cannot read, or whose **last byte is not a terminator** — the kernel counts arguments
by counting terminators, so an unterminated blob would run the final string off the end of
what it copied.

The kernel copies it into the child's address space as one page at `USER_ARGS_BASE`:

| offset | | |
|---|---|---|
| `0x000` | `u32` | `argc` |
| `0x004` | `u32 × argc` | byte offset of each argument, from the base of the page |
| then | | the strings, each NUL-terminated |

Offsets rather than pointers, because the blob is built in one address space and read in
another — a launcher can never hand a child a pointer into its own memory. The page is
**always mapped**, so a program with no arguments reads `argc == 0` instead of faulting on
its first look, and it is **writable**, so a program may chew on its own argument strings in
place. `USER_ARGS_BASE` is `0xBFFFF000`, the page directly above the user stack;
`USER_ARGS_MAX` is 4096 and `USER_ARGS_MAXC` is 64.

There is no `argc`/`argv` in the entry signature. X programs read the page through
`arg_count()` and `arg_at(i)` in the prelude, so `_start` keeps taking no parameters and no
program needs a runtime to unpack anything before `_start` runs.

> **Signature gap — closed by `exec_path` (§7.10, now implemented).** `spawn` takes an image
> **already in the caller's memory** and does **not** verify it; only the kernel's own load
> path calls `cxex_verify_trusted`. That was harmless while ring 3 had no filesystem, because
> the only images able to reach `spawn` arrived through a trusted build. `SYS_FILE_OP` ended
> that: a process can now read arbitrary bytes off a disk.
>
> `SYS_EXEC_PATH` (0x72) is the answer, and it closes the gap *by construction* rather than
> by bolting a check onto `spawn`: the kernel reads the file itself, so the bytes verified are
> exactly the bytes loaded and the caller never holds them. `spawn` keeps its meaning — run an
> image you already have and already trust — and remains how the executive starts its embedded
> shell.

### 7.8 Memory — `0x80–0x8F` (specified, unimplemented)

Relocated from v1's `0x20–0x22`, which input now occupies. Gated on `GRANT_MEM`.

| # | Name | Args | Returns |
|---|------|------|---------|
| 0x80 | `map`   | a1=virt, a2=phys, a3=flags | 0 |
| 0x81 | `unmap` | a1=virt | 0 |
| 0x82 | `sbrk`  | a1=delta (signed) | new break, or `-E_*` |

### 7.9 Storage — `0x90–0x9F` (specified, unimplemented)

Relocated from v1's `0x40–0x41`, which the framebuffer now occupies. Gated on `GRANT_DISK`.
64-bit LBAs travel inside the arg struct, never split across registers (§2).

| # | Name | Args | Returns |
|---|------|------|---------|
| 0x90 | `block_read`  | a1=`*block_args` | bytes read |
| 0x91 | `block_write` | a1=`*block_args` | bytes written |

```c
struct block_args { uint8_t disk_id; uint64_t lba; uint32_t count; void *buf; };
```

### 7.10 Filesystem & path execution — **implemented**

Both calls described here as proposals now exist. They landed at different numbers than
proposed, in the families they actually belong to rather than a new range:

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x60 | `file_op`   | a1=`*file_op_args` | `GRANT_DISK` | op-specific |
| 0x72 | `exec_path` | a1=path ptr, a2=`*spawn_args` (image fields ignored) | `GRANT_SPAWN` | new pid |

Two deviations from the proposal, both deliberate:

- **`file_op` is `0x60`, not `0xA0`, and is gated on `GRANT_DISK` rather than a new `GRANT_FS`
  bit.** `GRANT_DISK` was already defined and referenced by no syscall at all — it was reserved
  for exactly this and adding `GRANT_FS` beside it would have left two bits meaning the same
  authority.
- **`exec_path` is `0x72`, in the process family beside `spawn` (0x70) and `power` (0x71).**
  It is a process-launch call, not a filesystem call; grouping it with `fs_op` would have
  split the launch family across two ranges.

Offsets and sizes cross this boundary as **32-bit signed** values, because X Native has no
64-bit integer type. The kernel refuses anything past `INT32_MAX` rather than truncating, so
a user-addressable file tops out at 2 GB. CXFS remains 64-bit underneath.

- **`fs_op`** follows the established selector shape (`fb_op`, `net_op`) rather than adding
  a dozen numbers: ops for `STAT`, `OPEN`, `READ`, `WRITE`, `CLOSE`, `READDIR`, `MKDIR`,
  `UNLINK`, `RENAME`. Open files become a second handle type (`HANDLE_FILE`), which is what
  the handle table in §6 was built to absorb.
- **`exec_path`** loads apps from disk with signature verification inherited from the
  kernel's own load path, closing the `spawn` gap noted above. It shares `cxex_exec`'s verify
  and type checks through `cxex_exec_as()`; the two differ only in where authority comes
  from. `cxex_exec` is the kernel starting something by itself, with nobody to attenuate
  from, so `caps_for()` reads the image's tier — an `.xoex` gets `GRANT_OS_BASELINE`, an
  `.xuex` gets nothing. `exec_path` is a ring-3 process asking for a launch, so §5 attenuation
  applies instead. Running `caps_for()` there would make every program loaded from disk
  capability-less and unable to so much as print.
- **A negative `broker_endpoint` means no broker**, and the child gets no handle 0. `spawn`
  requires one because it was written for the brokered model; §1 records that this is now a
  spectrum. A shell holds a *SEND* handle to its own executive, not a *RECV* endpoint it
  owns, so requiring a broker would mean no shell could launch anything without first
  becoming a broker and serving the child's IPC itself.

---

## 8. IPC rendezvous semantics (synchronous, single in-flight)

The channel is the security boundary in model B, so its semantics are exact:

1. App calls `ipc_call(h, req, req_len, reply, reply_cap)`. The kernel:
   - validates `h` is a `HRIGHT_SEND` endpoint handle (`E_BADF` else),
   - validates `req`/`reply` buffers against the **app's** region (`E_FAULT` else),
   - bounds `req_len ≤ 0x1000` (`E_RANGE` else),
   - copies the request into a kernel bounce buffer,
   - **blocks the app** and marks the endpoint as having a waiting caller,
   - schedules the endpoint's owner.
2. The executive calls `ipc_recv(buf, cap, &sender)`: blocks until a caller is queued, then
   the kernel copies the request into `buf` (≤ `cap`, `E_RANGE` if too small) and writes the
   caller's pid into `*sender`. Returns the request length.
3. The executive does the work (using its caps) and calls `ipc_reply(data, len)`: the kernel
   copies `data` (≤ `0x1000`) into the **blocked caller's** reply buffer (≤ its `reply_cap`),
   wakes the caller; `ipc_call` returns the reply length.

- **One outstanding call per endpoint** in v1: the executive fully serves a request before
  the next `ipc_recv`. (Concurrent/queued IPC is v-next.)
- **The message bytes are opaque to the kernel.** The request/reply *schema* (request codes,
  field layout) is defined by the executive and shared with its apps — the ABI only moves
  bounded byte buffers across the wall. v1 cap is one page each way.

---

## 9. Allocation map & what remains reserved

Current state of the whole number space, so the next allocation is an informed one:

| Range | Owner | State |
|-------|-------|-------|
| `0x00–0x0F` | lifecycle | live (`0x00–0x03`) |
| `0x10–0x1F` | IPC + handles | live (`0x10–0x13`, `0x16`) |
| `0x20–0x2F` | input | live (`0x20–0x21`) |
| `0x30–0x3F` | console | live (`0x30`) |
| `0x40–0x4F` | framebuffer | live (`0x40`) |
| `0x50–0x5F` | network | live (`0x50`) |
| `0x60–0x6F` | shared-memory grant | reserved, unimplemented |
| `0x70–0x7F` | process | live (`0x70–0x71`) |
| `0x80–0x8F` | memory | specified, unimplemented (§7.8) |
| `0x90–0x9F` | storage | specified, unimplemented (§7.9) |
| `0xA0–0xAF` | filesystem + path exec | proposed (§7.10) |
| `0xB0+` | — | free |

Still genuinely reserved and unimplemented:

- `GRANT_IOPORT` (0x0080) — bit reserved, no primitive.
- Syscalls `0x14–0x15` (IPC async/notify), `0x17–0x1F` (handle dup/transfer), `0x60–0x6F`
  (shared-memory grant: `shm_create`, `shm_grant`, `shm_map`).
- Additional handle object types (`HANDLE_SHM`, more endpoints). `HANDLE_FILE` arrives with
  §7.10.

Delivered since v1 and therefore no longer deferred: `GRANT_NET` + the network primitive,
`GRANT_FRAMEBUFFER` + the framebuffer primitive, unprivileged input, and `spawn` requesting an
attenuated capability subset for the child.

---

## 10. Proof-of-life demo (the v1 acceptance test)

A capability-less ring-3 app asks its executive to print a string:

1. Executive (`GRANT_OS_BASELINE`) calls `ep_create` → endpoint handle `E`.
2. Executive `spawn`s the app with `broker_endpoint = E`; the kernel gives the app a SEND
   handle to `E` as its handle `0`, and `caps = 0`.
3. App calls `ipc_call(0, "hello\n", 6, reply, sizeof reply)` and blocks.
4. Executive `ipc_recv`s the request, calls `console_write("hello\n", 6)` (allowed —
   it holds `GRANT_CONSOLE`), then `ipc_reply` with a status.
5. App wakes with the reply.
6. **Negative check:** if the app calls `console_write` directly, it gets `E_PERM` — proving
   apps cannot reach privileged primitives, only the broker can.

That one flow exercises the wall, the cap gate, endpoints, the handle table, and the
synchronous rendezvous together.

---

## 11. Implementation checkpoints (build order, each host-verifiable)

> **Status: CP1, CP2 and CP3 are implemented.** The capability gate, the handle table with
> endpoints, and synchronous IPC with scheduler block/wake all exist and the §10 demo flow
> runs.
>
> **CP4 (hardening) is mostly closed as of 2026-10-09**, by Phase 2 of
> `docs/planning/HARDENING_PLAN.md` rather than as ABI work. Endpoints are reference
> counted and reclaimed, a closed endpoint is a dead name, a close mid-rendezvous wakes
> the peer with `E_BADF`, user copies are validated at the instant of the copy and a
> fault during one is recoverable, and the lifecycle sequences this used to call out —
> create, run, block, wake, exit, and the frames coming back — are driven at every boot
> by `test_lifecycle_sequences` and `test_ipc_endpoint_lifetime`.
>
> **What remains of CP4 is message-bound fuzzing**, which is still not done
> systematically: the length checks are asserted at their edges, but nothing generates
> adversarial message sizes across the three IPC entry points the way `ktest_loader.c`
> does for CXEX images. The next ABI work is not a checkpoint below but §7.10.

- **CP1 — caps + gate.** Per-process `caps` bitmask, `caps_for` at the `cxex_exec` handoff,
  the privileged primitives (`console_write`, `map/unmap/sbrk`, `block_read/write`, `power`)
  gated on their caps. Port the per-process user-pointer check and the user-fault hook
  from legacy. Executive can call primitives; a capless process gets `E_PERM`.
  *(Landed, and the check is now the split `user_ptr_readable` / `user_ptr_writable`
  pair rather than the single `user_ptr_ok` this line was written against — see §3.)*
- **CP2 — handle table + endpoints.** The 16-slot table, `ep_create`, `handle_close`, and
  `spawn` installing the child's broker handle 0 + forcing `caps=0`.
- **CP3 — synchronous IPC.** `ipc_call` / `ipc_recv` / `ipc_reply` with scheduler
  block/wake, leaning on the proven `enter_usermode` save-slot + per-process `esp0`
  (legacy `process_create_ring3`). This is where the demo lights up end to end.
- **CP4 — hardening.** Lifecycle edge cases (caller dies mid-call, executive dies with a
  caller blocked), handle leak/cleanup on exit, message-bound fuzzing.

Then v-next: shared-memory grants (bulk transfer), concurrent IPC, a second executive.

---

## 12. Constants quick-reference

| Name | Value |
|------|------:|
| Syscall vector | `int 0x80` (gate DPL 3) |
| Reg convention | eax=num, ebx/ecx/edx/esi/edi=args, eax=result |
| User CS / data | `0x1B` / `0x23`; kernel data `0x10` |
| Max user msg | `0x1000` (one page) each way |
| Handle slots | 16 per process |
| Broker handle | `0` (app's SEND handle to its executive's endpoint) |
| Executive type | `CXEX_TYPE_OS` = `0x4F45` |
| App type | `CXEX_TYPE_USER` = `0x4345` |
| `GRANT_OS_BASELINE` | `CONSOLE|MEM|DISK|NET|SPAWN|POWER|ENDPOINT|FRAMEBUFFER` = `0x017F` |
| Argument page | `USER_ARGS_BASE` = `0xBFFFF000`, one page, always mapped |
| Max argument bytes | `USER_ARGS_MAX` = `4096` (header included) |
| Max arguments | `USER_ARGS_MAXC` = `64` |
| Syscalls implemented | 16 |

---

*This is the v2 contract. The rule is: **a number is frozen once it ships.** Add new calls in
the free ranges in §9, keep every shipped number stable, and when the ABI revs, bump a version
constant the executive can query. Unimplemented reservations are advisory — if a range is
needed and its occupant was never built, move the occupant and say so here, as v2 did for
§7.8 and §7.9.*

*`abi/cxk_abi.h` is the source of truth. A consistency check between the header and this
document — asserting every `SYS_*` and every ABI struct appears in both — is worth having in
CI; the same one-source-of-truth argument the X language spec makes for the generated prelude
applies to the prose.*