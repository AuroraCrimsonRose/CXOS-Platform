# CXK ABI v1 — Frozen Syscall & Capability Contract

**Status:** FROZEN CONTRACT (v1). The numbers, register convention, capability bits,
handle/IPC semantics, and error codes below are stable. Both the `.xoex` executive and the
X/XR/XH toolchain compile against this. Additions go in reserved ranges; nothing in v1 is
renumbered or repurposed. Source basis: legacy v4 `cpu/usermode.{c,asm}` + `idt`, v6
`cpu/exec.c` / `lib/format/cxex_verify`.

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

### What v1 deliberately defers (all purely additive)
- **Shared-memory grants.** v1 moves bulk data as bounded, ≤1-page, **kernel-copied** reply
  payloads. Shared memory is a v-next primitive; the `ipc_call` shape already leaves room.
- **Multiple concurrent executives** (v1 launches one).
- **Concurrent in-flight IPC** (v1 endpoint serves one call at a time).
- **App self-service** (v1 apps are maximally thin — everything privileged goes through the
  broker).

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

- The kernel **never dereferences a ring-3 pointer** without validating it. Every user
  pointer argument is checked with `user_ptr_ok(ptr, len)` against the **calling process's**
  mapped user region, length-bounded. Failure → `E_FAULT`. (This is the per-process
  generalization of the legacy single-region `user_ptr_ok`.)
- Maximum single user transfer is bounded (one page, `0x1000`, for messages; primitives
  state their own caps).
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
cannot forge them. The dispatcher gate is one line: `if (!(cur->caps & CAP_X)) return E_PERM;`

| Bit | Name | Gates |
|----:|------|-------|
| 0x0001 | `CAP_CONSOLE`  | `console_write` |
| 0x0002 | `CAP_MEM`      | `map`, `unmap`, `sbrk` |
| 0x0004 | `CAP_DISK`     | `block_read`, `block_write` |
| 0x0008 | `CAP_NET`      | (reserved v1) network primitives |
| 0x0010 | `CAP_SPAWN`    | `spawn` |
| 0x0020 | `CAP_POWER`    | `power` |
| 0x0040 | `CAP_ENDPOINT` | `ep_create` (may own an IPC endpoint, i.e. may be a broker) |
| 0x0080 | `CAP_IOPORT`   | (reserved v1) raw port I/O / driver tier |

Always-available calls (lifecycle + `ipc_call` + `handle_close`) require **no** capability.

### `caps_for(type, trusted)` — the policy layer
```
caps_for(type, trusted):
    if (!trusted)                 return 0;                 // verified but untrusted: nothing
    if (type == CXEX_TYPE_OS)     return CAP_OS_BASELINE;   // a broker executive
    if (type == CXEX_TYPE_USER)   return 0;                 // apps are capability-less
    return 0;

CAP_OS_BASELINE = CAP_CONSOLE | CAP_MEM | CAP_DISK | CAP_SPAWN | CAP_POWER | CAP_ENDPOINT
```
- `CXEX_TYPE_OS` = `0x4F45`, `CXEX_TYPE_USER` = `0x4345` (from the CXEX type-code family).
- Apps are created via `spawn` (§7) by the executive; in v1 the kernel forces a spawned
  app's caps to **0**. (A later version may let `spawn` request a subset; v1 does not.)

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
| 0x13 | `ep_create`    | — | `CAP_ENDPOINT` | new endpoint handle (`HRIGHT_RECV`) |

### Handles — `0x16–0x1F`

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x16 | `handle_close` | a1=handle | none | 0 |

### Memory — `0x20–0x2F` (privileged)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x20 | `map`   | a1=virt, a2=phys, a3=flags | `CAP_MEM` | 0 |
| 0x21 | `unmap` | a1=virt | `CAP_MEM` | 0 |
| 0x22 | `sbrk`  | a1=delta (signed) | `CAP_MEM` | new break, or `-E_*` |

### Console — `0x30–0x3F` (privileged)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x30 | `console_write` | a1=buf ptr, a2=len (0 = bounded NUL-scan) | `CAP_CONSOLE` | bytes written |

### Storage — `0x40–0x4F` (privileged; 64-bit LBA via arg struct)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x40 | `block_read`  | a1=`*block_args` | `CAP_DISK` | bytes read |
| 0x41 | `block_write` | a1=`*block_args` | `CAP_DISK` | bytes written |

```c
struct block_args { uint8_t disk_id; uint64_t lba; uint32_t count; void *buf; };
```

### Process — `0x70–0x7F` (privileged)

| # | Name | Args | Caps | Returns |
|---|------|------|------|---------|
| 0x70 | `spawn` | a1=`*spawn_args` | `CAP_SPAWN` | new pid, or `-E_*` |
| 0x71 | `power` | a1=action (0 reboot, 1 shutdown) | `CAP_POWER` | does not return on success |

```c
struct spawn_args {
    const void *image;   uint32_t image_len;   /* the app's CXEX bytes */
    const char *name;
    int         broker_endpoint;               /* RECV handle the executive owns; the kernel
                                                  installs a SEND handle to it as the child's
                                                  handle 0. */
};
```

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

## 9. Reserved for v-next (do not allocate in v1)

- `CAP_NET` (0x0008), `CAP_IOPORT` (0x0080) — bits reserved, primitives unimplemented.
- Syscalls `0x14–0x15` (IPC async/notify), `0x17–0x1F` (handle dup/transfer), `0x50–0x5F`
  (net), `0x60–0x6F` (shared-memory grant: `shm_create`, `shm_grant`, `shm_map`).
- `spawn` requesting a non-zero capability subset for the child.
- Additional handle object types (`HANDLE_FILE`, `HANDLE_SHM`, more endpoints).

---

## 10. Proof-of-life demo (the v1 acceptance test)

A capability-less ring-3 app asks its executive to print a string:

1. Executive (`CAP_OS_BASELINE`) calls `ep_create` → endpoint handle `E`.
2. Executive `spawn`s the app with `broker_endpoint = E`; the kernel gives the app a SEND
   handle to `E` as its handle `0`, and `caps = 0`.
3. App calls `ipc_call(0, "hello\n", 6, reply, sizeof reply)` and blocks.
4. Executive `ipc_recv`s the request, calls `console_write("hello\n", 6)` (allowed —
   it holds `CAP_CONSOLE`), then `ipc_reply` with a status.
5. App wakes with the reply.
6. **Negative check:** if the app calls `console_write` directly, it gets `E_PERM` — proving
   apps cannot reach privileged primitives, only the broker can.

That one flow exercises the wall, the cap gate, endpoints, the handle table, and the
synchronous rendezvous together.

---

## 11. Implementation checkpoints (build order, each host-verifiable)

- **CP1 — caps + gate.** Per-process `caps` bitmask, `caps_for` at the `cxex_exec` handoff,
  the privileged primitives (`console_write`, `map/unmap/sbrk`, `block_read/write`, `power`)
  gated on their caps. Port the per-process `user_ptr_ok` and the user-fault hook from
  legacy. Executive can call primitives; a capless process gets `E_PERM`.
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
| `CAP_OS_BASELINE` | `CONSOLE|MEM|DISK|SPAWN|POWER|ENDPOINT` = `0x0077` |

---

*This is the v1 contract. When the kernel revs the ABI, bump a version constant the executive
can query, keep these numbers stable, and add new calls only in the reserved ranges above.*