# Syscalls, the ABI and IPC
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN, for x86-64.** The interface between programs and the
> kernel, and between programs. The ABI as built is `abi/cxk_abi.h` until this
> lands. The kernel side of IPC (queued calls, reply tokens, badges) is
> `kernel/CX_KERNEL_PLAN.md` §6; this document is the interface.

---

## 1. Where it stands

| Area | Today | Problem |
|---|---|---|
| Entry | `int 0x80`, a number and two arguments (`syscall_dispatch(num, a1, a2)`) | Slow on x86-64; two arguments force a structure for almost everything |
| Shape | 62 numbers, one operation each: step 6a2c split every multiplexer, `SYS_FILE_OP` last (§9.5) | Solved: a number hid up to 18 operations, so tracing, recording and filtering saw "a file call" and nothing more, and each multiplexer re-implemented dispatch |
| Structures | Raw pointers and `int` inside shared structures | Layout changes with pointer size; every structure changes on x86-64 |
| Widths | `ticks` in 32-bit milliseconds; timestamps 32-bit. Sizes and offsets are `u64` since step 6a2d (§9.5) | Ticks wrap after 49 days; dates stop in 2106. Solved for sizes: files stopped at 2 GB |
| Authority | `GRANT_*` bits on the process; handles are small `int`s with no rights | A handle carries no rights of its own; a stale handle number can name a new object |
| Devices | Drawing (`SYS_FB_DRAW_TEXT`), keyboard and mouse are syscalls; input needs no capability | The kernel draws pixels; any process can read another's keys |
| Arguments | A fixed page at `USER_ARGS_BASE` | An x86-32 address in the ABI |
| Errors | 15 codes | No timeout, peer-closed, wrong-type or rights-denied distinction |
| IPC | Synchronous, one page copied, one call in flight per endpoint; the executive defines messages informally | No handles in messages, no shared memory, no types |
| Definition | Since step 6a1, `abi/kernel.xfil`: `cxk_abi.h`, the X prelude's constants and structures, and its plain syscall wrappers are generated from it (§9.5). The prelude's helpers are by hand. Since 6a3a every image records the ABI hash and the loader compares it | The interface it describes is still this table's |

The good parts stay: user copies that validate and recover from faults
(`usermode.c`), capability attenuation at spawn, the broker model, and
`check-abi`.

## 2. How the others do it

- **Linux and Serenity:** one large flat table (Serenity has 158 entries in
  `Kernel/API/Syscall.h`), POSIX-shaped, file descriptors and signals.
- **NT:** about 400 `Nt*` calls in a generated service table
  (`KSERVICE_TABLE_DESCRIPTOR`, `ke.h`); handles with access masks; `NTSTATUS`
  codes; ALPC for messages.
- **Redox:** 36 syscalls (`syscall/src/number.rs`). Everything else is a
  "scheme" served by a user-space daemon, reached by opening a path; the
  operations become messages (`schemev2.rs`).
- **XNU:** Mach messages carrying port rights and out-of-line memory
  (`ipc_kmsg_copyin_*_descriptor`, `osfmk/ipc/ipc_kmsg.c`); MIG generates the
  stubs from `.defs` files (46 in `osfmk/mach`). BSD syscalls alongside.
- **Haiku:** `gensyscalls` generates the table from a header; services talk
  through ports and `BMessage`.
- **Serenity IPC:** `.ipc` files (`WindowServer/WindowClient.ipc`) compiled
  into C++ client and server classes.
- **Fuchsia** (public, not in the tree): about 170 syscalls, every one an
  operation on a handle with rights; all defined in one interface language;
  programs may only enter the kernel through the vDSO; every service is a typed
  FIDL protocol. The closest model for CXOS.

## 3. Principles

1. **Small, handle-shaped kernel interface.** Almost every syscall takes a
   handle first and is checked against that handle's rights. One number per
   operation: no multiplexers.
2. **Everything else is a typed service over IPC.** Display, input, audio,
   network configuration, settings, the Process Manager. The GUI and the CLI
   are both clients (roadmap: GUI first, never GUI only).
3. **One definition generates everything.** Syscalls and service interfaces
   are written once (§9) and generate the C header, the kernel dispatch table,
   the X bindings, the C# prelude, the trace decoder and the reference pages.
   Nothing is kept in step by hand.
4. **No ambient authority.** No syscall takes a pid, a global name or an
   absolute path as authority. Paths are always relative to a directory handle.
5. **Every blocking call takes a deadline**, absolute, on the monotonic clock.
6. **Fixed widths everywhere.** The ABI means the same thing to C, X and C#.

## 4. Calling convention (x86-64)

- **Entry:** the `syscall` instruction. Number in `rax`; arguments in `rdi`,
  `rsi`, `rdx`, `r10`, `r8`, `r9` (`rcx` and `r11` are taken by the
  instruction). Six register arguments, so most calls need no structure.
- **Return:** `rax` holds a signed 64-bit status: zero or a positive value on
  success, a negative error code on failure. Further results go through
  output pointers.
- **Only from the system page.** The kernel maps a read-only system page into
  every process (§8) holding the syscall stubs, and refuses a `syscall` whose
  return address lies outside it (Fuchsia's vDSO rule; OpenBSD pins syscalls
  to libc the same way). Injected code cannot call the kernel directly, and
  numbers can change freely because only the system page knows them.
- **Kernel side:** `swapgs`, a per-CPU kernel stack, SMAP's `stac`/`clac`
  around user copies where CPUID reports SMAP. Page-table isolation (KPTI)
  only on CPUs that need it; AMD CPUs, the rig's included, do not.
- **User memory** is always a pointer and a length, validated once at entry
  (the `*user` slice of `language/CX_X_PLAN.md` §3) and copied with the
  fault-recovering copy that exists today.
- **One recording point:** every call passes one function on entry and exit
  (`kernel/CX_KERNEL_PLAN.md` §2.1). Tracing, record and replay, and
  per-syscall statistics hang off it.

## 5. ABI rules

- Little-endian. Integers have explicit widths. Sizes, offsets and counts are
  `u64`. User addresses inside structures are `u64`, never pointer types.
- Structures have no implicit padding; padding is a named field that must be
  zero. The generator checks this.
- Booleans are `u8` (0 or 1; anything else is `E_INVALID_ARGS`). Enumerations
  are `u32`. Unknown flag bits are refused, never ignored, so a flag can be
  given a meaning later without old programs having passed garbage in it.
- Strings are a pointer and a length, UTF-8, not NUL-terminated.
- **Time** is `i64` nanoseconds: the monotonic clock since boot, and UTC since
  1970 as a separate clock. Neither wraps in practice.
- **Handles** are `u32`: an index into the process's table plus generation
  bits, so a closed handle's number does not silently name the next object put
  in that slot. 0 is never a valid handle.
- **The image records the ABI's hash.** The loader refuses a program built
  against a different ABI with a clear message, instead of letting it run
  against the wrong numbers.

## 6. Errors

One list, generated, shared by syscalls and services. Each syscall's
definition lists the errors it may return; debug kernels check the returned
error against that list.

| Code | Meaning |
|---|---|
| `E_OK` | |
| `E_INTERNAL` | a kernel bug; never expected |
| `E_NOT_SUPPORTED` | no such operation (an unknown syscall number included), or not for this object |
| `E_INVALID_ARGS` | malformed arguments, unknown flags, bad alignment |
| `E_BAD_ADDRESS` | user memory not mapped or not accessible |
| `E_BAD_HANDLE` | not a live handle in this process |
| `E_WRONG_TYPE` | the handle names the wrong kind of object |
| `E_ACCESS_DENIED` | the handle lacks a right, or policy forbids it |
| `E_NO_MEMORY` | commit refused (strict commit, `kernel/CX_KERNEL_PLAN.md` §9.4) |
| `E_NO_RESOURCES` | a table or budget is full |
| `E_OUT_OF_RANGE` | an offset or size beyond the object |
| `E_BUFFER_TOO_SMALL` | the output buffer is too small; the size needed is returned |
| `E_TIMED_OUT` | the deadline passed |
| `E_CANCELED` | the wait or call was canceled |
| `E_PEER_CLOSED` | the other end is gone |
| `E_SHOULD_WAIT` | nothing ready now; wait and retry |
| `E_BUSY` | in use; try later |
| `E_ALREADY_EXISTS`, `E_NOT_FOUND` | names |
| `E_NOT_DIR`, `E_IS_DIR`, `E_NO_SPACE`, `E_IO`, `E_CORRUPT` | storage |
| `E_DEADLOCK` | as today, for file locks |
| `E_PROTOCOL` | an IPC message did not match its interface |

There is no `EINTR`: with no signals, nothing interrupts a call.

**As built (roadmap step 6a2b, `CXK_ABI_VERSION` 4).** The values are negative,
`E_OK` 0 and the rest -1 to -25 in the table's order, in `abi/kernel.xfil`.
Each old code became the new one that names the same condition, chosen per
site where one old code covered several: `E_RANGE` is `E_BUFFER_TOO_SMALL` for
an output buffer (object info, the MAC, a receive, the cwd) and
`E_OUT_OF_RANGE` elsewhere; `E_NOMEM` is `E_NO_RESOURCES` for a full table
(handles, endpoints, open files, mapping slots, address space), `E_NO_SPACE`
for a full volume, and `E_NO_MEMORY` for frames, heap and the commit quota;
`E_AGAIN` is `E_BUSY` (a lock conflict, an endpoint with a call in flight);
an endpoint closed under a waiting call is `E_PEER_CLOSED`, under a waiting
receive `E_CANCELED`. No call returns the size needed with
`E_BUFFER_TOO_SMALL` yet, and nothing yet returns `E_WRONG_TYPE`,
`E_TIMED_OUT`, `E_SHOULD_WAIT`, `E_CORRUPT` or `E_PROTOCOL`.

## 7. The syscalls

About 60 operations, grouped by object. Names are `object_verb`.

**Handles and objects:**
`handle_close`, `handle_duplicate(h, rights)` (rights can only shrink),
`handle_replace(h, rights)`, `object_info(h, topic, buf)` (type, rights, kernel
id, name, grant chain, statistics), `object_set_name`.

**Waiting and time:**
`object_wait(h, signals, deadline)`, `queue_create`, `queue_register(q, h,
signals, key)`, `queue_wait(q, events, max, deadline)`, `queue_cancel`,
`event_create`, `object_signal(h, clear, set)` (events and user-defined
signals), `timer_create(clock, slack)`, `timer_set(t, deadline, period)`,
`timer_cancel`, `clock_read(clock)` (normally served from the system page
without entering the kernel).

**Processes, threads and jobs:**
`process_spawn(spec)` (§10), `process_exit(code)`, `process_stop(p, deadline)`,
`process_kill(p)`, `thread_create(p, entry, stack, arg)`, `thread_exit`,
`thread_set_priority`, `thread_yield`, `job_create(parent, limits)`,
`job_set_limits`. A thread waits on a process, thread or job handle to learn
how it ended (`kernel/CX_KERNEL_PLAN.md` §4).

**Memory:**
`mem_create(size, flags)`, `mem_read`, `mem_write`, `mem_set_size`,
`mem_seal(m, seals)`, `vm_map(m, offset, len, prot, flags)`,
`vm_unmap(addr, len)`, `vm_protect(addr, len, prot)`.

**Synchronisation:**
`futex_wait(addr, expected, owner, deadline)`, `futex_wake(addr, count)`,
`futex_requeue`.

**IPC** (§11):
`channel_create` (returns a receive handle and a send handle),
`channel_mint(send, badge)`, `ipc_call(send, msg, handles, reply, deadline)`,
`ipc_send(send, msg, handles)` (one way), `ipc_receive(recv, buf, handles,
deadline)` (returns a reply token and the badge), `ipc_reply(token, msg,
handles)`.

**Streams and files:**
`pipe_create(size)`, `io_read(h, buf, offset)`, `io_write(h, buf, offset)`
(pipes ignore the offset), `io_seek`, `fs_open_at(dir, path, flags, rights)`,
`fs_create_at`, `fs_unlink_at`, `fs_rename_at(dir, path, dir2, path2)`,
`fs_mkdir_at`, `file_stat`, `file_set_size`, `file_sync`, `dir_read`,
`file_lock`, `file_unlock`.

**Drivers** (Phase 2, `kernel/CX_DRIVERS.md`):
`interrupt_bind(resource)` (an interrupt handle; waiting on it waits for the
interrupt), `interrupt_ack`, `ioport_bind(resource, range)`,
`mmio_map(resource, range)` (a memory object over device registers),
`dma_alloc(size, below_4g)` (a pinned memory object with its physical
addresses).

**System:**
`random_read`, `log_write` (to the kernel log, replacing `SYS_CONSOLE_WRITE`),
`power_set(resource, state)`, `system_info(topic, buf)`.

The x86-32 kernel has a first step towards `log_write`: a process started with
`SPAWN_BACKGROUND` (the supervisor, and every service through it) has its
`SYS_CONSOLE_WRITE` turned into log lines, and `SYS_LOG_READ` reads the ring
back (the shell's `log`). Once the executive runs, kernel lines below ERR stop
going to the screen as well.

**Retired:** the `SYS_FB_*` calls (the display server draws, into memory objects);
`SYS_INPUT_READ`, `SYS_INPUT_POLL` and `SYS_MOUSE_READ` (the input driver sends events to the
display server, which routes them to the focused window); `SYS_GETPID` and
`SYS_GETUID` (`object_info` on the process handle, or the system page);
`SYS_ARGS` and `USER_ARGS_BASE` (the start message, §10); `SYS_EXEC_PATH`
(spawn takes a file handle); every `*_OP` multiplexer.

## 8. The system page

Mapped read-only in every process at an address the loader picks.

- The syscall stubs, the only code the kernel accepts `syscall` from.
- Clock data under a sequence counter (TSC scale and offset), so reading
  either clock costs no kernel entry.
- The CPU feature table (`kernel/CX_KERNEL_PLAN.md` §2.3), the page size, the
  ABI hash.
- Thread-local data is reached through `FS_BASE`, not the system page.

## 9. One definition for everything

Syscalls and service interfaces are written in one small interface language,
**X Format Interface Language**, in `.xfil` files (decided 2026-10-16), parsed
by the DevKit:

```
xfil 1
library cx.display
domain system

interface Window {
    set_title(title: string[256]) -> ()
    resize(width: u32, height: u32) -> result<(), Error>
    attach_buffer(buffer: handle<memory, read | map>) -> ()
    event closed()
}
```

### 9.1 The header decides the domain

The extension is always `.xfil`: under the naming formula, `F` is the source
domain, the same for every source file (`.xfxn` does not say whether X Native
code is for the kernel or an app either). **Whose an interface is lives in the
file's header**, the rule CXOS already applies to executables: the extension is
a hint, the header decides (`formats/CX_FILE_TYPES.md` §1).

Every `.xfil` file starts with three lines:

| Line | Meaning |
|---|---|
| `xfil 1` | the language version, so the generator can refuse a file it does not understand |
| `library <name>` | the namespace, reverse-dotted; each library is owned by one signing key |
| `domain <tier>` | whose the interface is, using the executable ladder's tiers |

The domain changes what is generated and what is enforced:

| Domain | Mirrors | Who serves it | Generated | Enforced |
|---|---|---|---|---|
| `kernel` | `.xkex` | the kernel | syscall numbers, the C dispatch table and argument decoding, the system-page stubs, X Native wrappers | Only the platform tree may contain one; there is exactly one (`cx.kernel`) |
| `boot` | `.xbex` | the loader | `cxk_boot.h` (CXBI) and its X and C# views | Only the platform tree; fixed layout, no messages |
| `os` | `.xoex` | the executive | IPC stubs | Served only by the platform-signed executive (the broker itself) |
| `system` | `.xsex` | OS-owned services: display, network, files, audio | IPC stubs | The broker registers a server for a `system` interface only if its image is a platform-signed `.xsex` |
| `user` | `.xuex` | any app, for other apps (plugins, sharing) | IPC stubs | The library name must sit under its publisher's namespace, and only that publisher's key may serve it |

A file holds one library and one domain. Mixing kernel and user interfaces in
one file would make every review and every generator decide per item; one
domain per file keeps the boundary visible in the file list.

### 9.2 Interfaces in the executable

The generator also records, for each program, the interfaces it **serves** and
the interfaces it **uses**: library, interface, domain and a hash of the
definition. `cxk build` writes this list into the CXEX image as a section
covered by the signature.

- **The loader** refuses an image that claims to serve an interface above its
  own tier (a `.xuex` serving a `system` interface).
- **The broker** compares hashes when it connects a client to a server; a
  mismatch is a clear "built against a different interface" error instead of
  `E_PROTOCOL` at the first call.
- **The Process Manager and the install screen** show what an app talks to
  before it runs, read from the signed list.

The ABI hash of §5 is this hash for the `kernel` library.

### 9.3 Bindings

- **X Native** (`.xfxn`) is the main client and server language: the
  generator writes `.xfxn` stubs. The generated prelude, `abi.x` today, becomes
  `abi.xfxn`, which also fixes the naming inconsistency recorded in
  `devkit/CX_DEVKIT_DESIGN.md` §3.
- **The kernel** (`kernel.xkex`, C) gets the C side of the `kernel` domain: the
  dispatch table, argument structures and decoders. It never links a
  service's stubs.
- **C# (the DevKit)** gets the constants, structures and the trace decoder.
  The generator lives in `cxk`, so a stale `cxk` generates stale files;
  `check-abi` compares them with the committed ones and fails by name, as it
  does today.

### 9.4 Rules

- **Types:** fixed-width integers, `bool`, enums, bit sets, bounded strings and
  arrays, structures, `result<T, E>`, and handles with their object type and
  the rights they must carry.
- **Generated:** the C header and kernel dispatch table, X client stubs and
  server dispatch, the C# prelude, the trace decoder, the reference pages.
  `check-abi` becomes: the generated files match the committed ones.
- Generated decoders validate every message (lengths, enums, UTF-8, handle
  types and rights) before server code sees it. A malformed message is
  `E_PROTOCOL` and never reaches a handler.
- Supersedes the rule that `cxk_abi.h` and `AbiPrelude.cs` change in one
  commit: both are generated from the same file.

### 9.5 As built (roadmap steps 6a1, 6a2a, 6a2b, 6a2c, 6a2d, 6a3a, 6a3b, 6a3c)

The `kernel` domain is generated; the other domains, and `interface` blocks
with their messages, are settled when IPC v3 needs them (Phase 1). The source
is `abi/kernel.xfil` (`library cx.kernel`), which describes the ABI as it
stands (no multiplexers since 6a2c; sizes and offsets `u64` since 6a2d). `cxk abi generate` writes
`abi/cxk_abi.h` from it, and since 6a2a the kernel's dispatch side as well:
`kernel/cpu/systable.h` (one handler prototype per syscall, named after it:
`SYS_FILE_READ` is `syscall_file_read`) and `kernel/cpu/systable.c` (a decoder per
syscall, giving each argument register its declared type, and the table,
indexed by number). `syscall_dispatch` takes the number and all five argument
registers from either gate at the register width (`uintptr_t`), refuses a number past the table or in one of its
holes with `E_NOT_SUPPORTED`, and the recording point keeps the last entry's
registers. `CXEX.Lang` embeds the `.xfil`, and the X prelude's constants,
structures and plain syscall wrappers come from the embedded copy
(`CXEX.Lang/Abi/Xfil*.cs`). `cxk check-abi` fails while any committed
generated file is not exactly what the `.xfil` beside it generates, or while
`cxk` was built from a different `.xfil`.

The language is line-oriented: one declaration, member or field per line.

```
xfil 1
library cx.kernel
domain kernel

/// documentation, carried into the generated files
// a comment that stays in the .xfil
const NAME: u32 = 0x10u              /// trailing note; u32 or i32
const BOTH: u32 = A | B              /// a literal, or an OR of earlier constants
enum mode {                          /// u32 constants; every value written out
    MODE_A = 0
}
struct file_stat {                   /// u8..u64, i8..i64, char[N], T[N]
    id: u32
    name: char[FILE_NAME_MAX]
}
syscall SYS_CLOCK = 0x04 fn clock_read(c: *clock_info) -> i32
syscall SYS_EXAMPLE = 0x3F0 (op: u32, ms: u32) -> i32   /// no plain wrapper
```

- Documentation lines directly above an item belong to it; a block followed
  by a blank line stands on its own (a section heading). Text that would end
  a C comment is refused.
- Values keep the tokens they were written with in the header (`0x10u` stays
  unsigned, `-1` becomes `(-1)`), so the kernel compiles to the same bytes.
- The parser computes each structure's layout and refuses implicit padding,
  inside or at the end; the header asserts every size it computed. A
  structure field is never a pointer.
- Every syscall declares its arguments, since the dispatch table decodes
  them; `fn name` adds a plain X wrapper. An argument is `u32`, `i32`, `u64`,
  `i64` or a pointer (`*u8`, `*u32`, `*u64`, `*struct`), and a syscall returns
  `i32` or `void`. Arguments are counted in x86-32 registers, at most five: a
  `u64` or `i64` takes two, low then high, so `(h, start: u64, len: u64)` fits
  and three `u64`s do not. Its name starts with `SYS_`, its number is its own
  and at most `0x3FF`. A wrapper passes a `u32` as it is, a `u64` as `x as u32,
  (x >> 32) as u32` (X targets x86-32 alone until step 6b), anything else
  through `as u32`. A decoder reads the registers as `uintptr_t` and casts each
  to its parameter's C type; one with a wide parameter is written twice, `#if
  SYSCALL_PAIRS` joining the pair on x86-32 and taking one register on x86-64.
  The kernel receives a pointer as a `uint32_t` user address. Wrappers come out in file order, and
  since every prelude function is compiled into every program, so do the
  programs' bytes.
- Every refusal names its line. Nothing is skipped: a generator that dropped
  what it could not read would drop ABI surface.

Step 6a2c retires the multiplexers a family at a time, each family an ABI
version of its own. A call keeps its argument structure only where its
arguments do not fit in registers; the `op` field and the outputs of other
operations leave it.

- **Memory** (`CXK_ABI_VERSION` 5): `SYS_MEM_OP` becomes `SYS_VM_MAP`
  (0x80, `*vm_map_args`: object, 64-bit offset, length, hint, protection and
  flags in, `addr` and `granted` out), `SYS_VM_UNMAP` (0x81, `addr, length`)
  and `SYS_VM_INFO` (0x82, `*vm_usage`). The object and offset stay until
  memory objects replace them, so an unknown object or a non-zero offset is
  still refused.
- **Power and input** (`CXK_ABI_VERSION` 6): `SYS_POWER`'s op becomes the
  number: `SYS_POWER_REBOOT` (0x90), `SYS_POWER_SHUTDOWN` (0x91) and
  `SYS_POWER_SLEEP` (0x92, `ms`), each behind `GRANT_POWER` and returning
  only when refused (sleep: after it). `SYS_INPUT_READ`'s flag becomes the
  number: `SYS_INPUT_READ` (0x20) blocks for a key, `SYS_INPUT_POLL` (0x22)
  answers 0 when none waits. Every call has its plain wrapper now, so the
  prelude's `reboot`, `shutdown`, `sleep`, `input_read` and `input_poll` are
  generated; `reboot` and `shutdown` return the refusal rather than `void`.
- **Framebuffer** (`CXK_ABI_VERSION` 7): `SYS_FB_OP` and `fb_op_args` go;
  each op is its own number, its arguments in registers: `SYS_FB_INFO`
  (0x40, `*fb_geometry` out), `SYS_FB_CLEAR` (0x41), `SYS_FB_FILL_RECT`
  (0x42), `SYS_FB_PUT_PIXEL` (0x43), `SYS_FB_DRAW_LINE` (0x44),
  `SYS_FB_DRAW_TEXT` (0x45, `x, y, text, fg, bg`, the characters drawn) and
  `SYS_FB_SET_FONT` (0x46, a name). One `GRANT_FRAMEBUFFER` gate; each
  answers `E_NOT_FOUND` with no framebuffer. `os/std/gfx.xfxn` calls the
  generated wrappers.
- **Network** (`CXK_ABI_VERSION` 8): `SYS_NET_OP`, `net_op_args` and
  `NET_OP_*` go. `SYS_NET_STATUS` (0x50), `SYS_NET_MAC` (0x51, `buf, len`),
  `SYS_NET_GET_CONFIG` (0x52, `*net_ip_config` out, which replaces the u32[4]
  buffer), one call per field for what `NET_OP_SET_IP`'s selector chose:
  `SYS_NET_SET_IP`, `_MASK`, `_GATEWAY`, `_DNS` (0x53-0x56), then
  `SYS_NET_PING` (0x57, `dst, seq, *rtt`), `SYS_NET_SEND` (0x58) and
  `SYS_NET_RECV` (0x59). One `GRANT_NET` gate. The `.xfil` now takes a pointer
  to an integer type (`*u32`) as well as to `u8` or a structure.
  `os/std/net.xfxn` keeps only its conveniences over the generated wrappers.
- **Files** (`CXK_ABI_VERSION` 9): `SYS_FILE_OP`, `file_op_args` and
  `FILE_OP_*` go. Eighteen calls did not fit 0x60-0x6F, so files move to a
  32-number block, 0xA0-0xBF, in the old op order: `SYS_FILE_OPEN` (0xA0,
  `path, flags`), `_CLOSE`, `_READ` and `_WRITE` (`h, buf, n`), `_SEEK` (`h,
  off, origin`), `_TELL`, `_TRUNCATE` (`h, size`), `_STAT` (`path, *file_stat`),
  `_FSTAT`, `_READDIR` (`path, index, *file_stat`), `_MKDIR`, `_UNLINK`,
  `_RENAME` (`path, newname`), `_CHDIR`, `_GETCWD` (`buf, cap`), `_SYNC`,
  `_LOCK` (`h, start, len, mode`) and `_UNLOCK` (0xB1, `h, start, len`). Every
  argument fits in registers, so no structure is left. One `GRANT_DISK` gate.
  The wrappers take the names `os/std/file.xfxn` had (`file_open`, `stat`,
  `dir_entry`, `file_lock_range` ...), so no caller changed; the file keeps
  only its conveniences. With this family the multiplexers are gone.

Step 6a2d (`CXK_ABI_VERSION` 10) makes every size and offset a `u64`; an
address stays `u32` until step 6b, and so do counts, indices and times (time
is a step of its own).

- **Arguments:** `console_write`'s `len`, `net_mac`'s and `net_send`'s `len`,
  `net_recv`'s `cap` (cut to 65535, more than any frame), `vm_unmap`'s
  `length`, `file_read`/`file_write`'s `n`, `file_truncate`'s `size`,
  `getcwd`'s `cap` and `file_unlock_range`'s `start, len`. A length past what
  the call can take is `E_OUT_OF_RANGE`, never cut to its low half.
- **Results:** a result is still an `i32`, so `file_seek` (`h, off: i64,
  origin, *u64`) and `file_tell` (`h, *u64`) return `E_OK` and write the
  position out; a bad pointer is refused before the offset moves. One read or
  write moves at most 2 GB - 1 bytes (`n` is cut to that) because its count
  is the result.
- **`SYS_FILE_LOCK`** (`h, *file_lock_args`): `h`, two `u64`s and a mode are
  six x86-32 registers, so the range goes in a structure. The generated wrapper
  is `file_lock_raw`; `os/std/file.xfxn` keeps `file_lock_range(h, start, len,
  mode)` over it.
- **Structures:** every length field is `u64` and leads its structure, so
  nothing is padded: `spawn_args` (`image_len`, `args_len`),
  `object_info_args` (`cap`), `ipc_call_args` (`req_len`, `reply_cap`),
  `ipc_recv_args` (`cap`), `ipc_reply_args` (`len`), `vm_map_args` (`offset`
  as one `u64`, `length`, `granted`), `vm_usage` and `file_stat.size`. Where
  the fields left a hole, a `reserved` field fills it, and a non-zero one is
  `E_INVALID_ARGS`, so it can be given a meaning later.
- **The kernel:** the 2 GB cap is gone. An open file's offset and its lock
  ranges are `uint64_t`, bounded by `INT64_MAX`; a write past it, a seek
  before 0 and a truncate or lock past it are `E_OUT_OF_RANGE`.

Step 6a3a records the ABI hash of §5 in every image. The hash is SHA-256 of
the `.xfil`'s definition: each line loses everything from its first `//`
(documentation included), whitespace runs become one space, and empty lines
go (`XfilParser.CanonicalText`). Rewording a comment leaves the hash alone,
and any change to a name, number, type or field changes it.

- **Generated:** `cxk abi generate` writes it into `cxk_abi.h` as
  `CXK_ABI_HASH`, a 32-byte initializer. The DevKit stamps the hash of its
  embedded `.xfil`, which `check-abi` already holds to the tree's.
- **The container:** CXEX format 2. The header is 88 bytes: the 56 bytes of
  fields as before (`phys_base` stays at 48 for the boot loader), then the
  hash at offset 56. The section table follows at 88. The writer refuses a
  layout without all 32 bytes.
- **The loader:** with `REQUIRE_ABI_MATCH` (required since the loader first
  checked compatibility), it compares `abi_version` first and then the hash.
  The same version over a different definition is `CXEX_LOAD_BAD_ABI_HASH`,
  so an ABI edited without a version bump is still refused. `abi_version`
  stays because a number is what an error message can show.

Step 6a3b puts every call through the system page of §8 (`CXK_ABI_VERSION`
11, which adds `SYSPAGE_STUB_SIZE`).

- **The page:** one stub per syscall, 16 bytes apart; stub n is
  `mov eax, n; int 0x80; ret` (`syscall` on x86-64), and a number with no
  syscall is int3 throughout. Each process gets its own copy, mapped
  read-only and executable and charged to its quota like the stack
  (`kernel/cpu/syspage.c`). It sits under the stack's guard page,
  `0xBFFBD000` today; the loader refuses an image section above
  `USER_IMAGE_END` (`0xBFF00000`), so nothing is mapped over the stubs, the
  stack or the arguments.
- **Entry:** ring 3 starts with every register zero but the base: `esi` on
  x86-32, `rsi` on x86-64 (x86-32 used to start with the kernel's registers).
- **The compilers:** `_start` keeps the base in `__cx_sysbase` before its
  prologue, and `__syscall(n, ...)` loads the arguments as before, then
  `shl $4, %eax; add __cx_sysbase, %eax; call *%eax`. A program built
  without the prelude is not a CXK program (the compiler's Linux host layer),
  and keeps `int $0x80`: `--no-prelude` in both compilers turns it off.
- **The C programs** (`executive.c`, `hello.c`) save `esi` in a naked
  `_start` and call `sysbase + n * SYSPAGE_STUB_SIZE`.
- The gate still accepted `int 0x80` from anywhere until 6a3c.

Step 6a3c makes the stubs the only way in (no ABI change).

- **The check:** the gate's first step, before the call is counted or
  dispatched, is `syscall_check_origin(num, ip)` (`kernel/cpu/usermode.c`).
  `ip` is the return address the CPU recorded, the `int 0x80` frame's EIP
  or the `rcx` that `syscall` loads, so ring 3 cannot forge it. It must be
  `SYSPAGE_VIRT + num * SYSPAGE_STUB_SIZE + 7`, the `ret` after the trap in
  that number's stub (`arch_syscall_stub_ret`), and the number is bounded by
  the table first, so a product that wraps cannot name a real stub. A trap
  from anywhere else is refused, and so is a jump into a stub's trap with
  another number in `eax`.
- **A refusal** ends the process the way a fault does: logged, noted for the
  crash area, and back out of `enter_usermode` with `USERMODE_FAULTED`. It is
  counted apart (`syscall_refused_count`), and it never reaches the recording
  point or the trap witness.
- **The kernel's own ring-3 helpers** (`process_create_ring3`,
  `usermode_run_blob`) share the kernel's address space and run in it
  several at once, so its system page is counted: the first to enter ring 3
  maps it, the last to leave unmaps it, and the space's user half is empty
  between runs, as x86-64 requires. A process with a space of its own maps its
  own (`spawn.c`, the x86-64 arch tests). Every test routine calls through the
  stubs and ends in `ud2`.
- **Tests:** `usermode_syspage_test` runs a direct `SYS_EXIT`, a `SYS_GETPID`
  through the `SYS_EXIT` stub's trap, and the same with `0x10000000`; each
  must be refused, with nothing dispatched.

When the `.xfil` replaced the hand-written header, the header it generated
matched the old one in every constant, structure and field type, and every
kernel and program built from it, both kernels included, came out
byte-identical.

## 10. Starting a process

`process_spawn` takes the program as a **file handle or memory object**, the
job to run in, the handles to pass (each named; nothing is inherited), and the
capabilities, attenuated against the caller's as today. It returns a process
handle and a thread handle.

The new process starts with one handle in a register: a channel holding its
**start message**, with its arguments, environment, and the named handles it
was given (its directory handles, standard input, output and error, and the
broker channel). The fixed argument page goes away. Fuchsia's `processargs`
works this way.

**Finding services:** a program asks the broker (the executive) for a service
by interface name. The broker checks policy and returns a send handle,
optionally with a badge identifying the caller. This is today's broker,
generalised.

## 11. IPC messages

- **A message** is a header (interface, method ordinal, flags), up to 64 KiB of
  bytes, and up to 16 handles carried beside the bytes, never inside them.
  Larger data goes in a memory object, sealed when the receiver must be able
  to trust it (`kernel/CX_KERNEL_PLAN.md` §6).
- **One copy.** When the receiver is already waiting, the kernel copies
  straight from the sender's buffer into the receiver's; otherwise into a
  kernel buffer charged to the sender's job.
- **Handles move** by default; a copy needs the duplicate right, and rights
  can only shrink in transit (XNU `ipc_kmsg_copyin_port_descriptor`).
- **Reply tokens, badges, several calls in flight, priority passed through
  calls:** as in `kernel/CX_KERNEL_PLAN.md` §6.
- **Transport errors are syscall results** (`E_PEER_CLOSED`, `E_TIMED_OUT`);
  **application errors are part of the reply type** (`result<T, E>`).
- **Events** (`event closed()` above) are one-way messages from server to
  client.
- Later, for speed: messages of a few words passed in registers (L4, seL4).

## 12. Order

| When | Work |
|---|---|
| Phase 0.1 | Handle table with rights and generations; `handle_close`, `handle_duplicate`, `object_info`; the error list |
| Phase 0.5 | `syscall` entry, `swapgs`, the per-CPU stack, SMAP where present |
| Phase 0.6 | **The ABI break.** Every program is rebuilt for x86-64 here anyway, so the new numbering, widths, the system page, the `.xfil` generator and the ABI hash land together |
| Phase 1 | Waiting, timers, IPC v3, pipes, threads, futex, jobs, `process_spawn` with the start message, the broker by interface name |
| Phase 2 | Driver syscalls; the display server and input service; `FB_OP` and the input syscalls removed |

## 13. Open questions

- Settled 2026-10-16: the network stack is a user-space service
  (`system/CX_NETWORK.md`); CXFS stays in the kernel and foreign filesystems
  run in user space (`system/CX_FILESYSTEM.md` §6).
- The `.xfil` syntax for the `kernel` domain is settled (§9.5, step 6a1).
  The `interface` syntax of §9 is still a sketch; it is settled with IPC v3.
