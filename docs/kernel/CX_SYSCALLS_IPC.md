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
| Shape | 22 numbers, four of them multiplexers (`SYS_FILE_OP`, `SYS_NET_OP`, `SYS_FB_OP`, `SYS_MEM_OP`) with an `op` field inside a structure | One number hides 18 file operations, so tracing, recording and filtering see "a file call" and nothing more; each multiplexer re-implements dispatch |
| Structures | Raw pointers and `int` inside shared structures | Layout changes with pointer size; every structure changes on x86-64 |
| Widths | `ticks` in 32-bit milliseconds; file sizes and offsets 32-bit; timestamps 32-bit | Ticks wrap after 49 days; files stop at 4 GB; dates stop in 2106 |
| Authority | `GRANT_*` bits on the process; handles are small `int`s with no rights | A handle carries no rights of its own; a stale handle number can name a new object |
| Devices | Drawing (`FB_OP_DRAW_TEXT`), keyboard and mouse are syscalls; input needs no capability | The kernel draws pixels; any process can read another's keys |
| Arguments | A fixed page at `USER_ARGS_BASE` | An x86-32 address in the ABI |
| Errors | 15 codes | No timeout, peer-closed, wrong-type or rights-denied distinction |
| IPC | Synchronous, one page copied, one call in flight per endpoint; the executive defines messages informally | No handles in messages, no shared memory, no types |
| Definition | `cxk_abi.h` by hand; the X prelude's constants and structures generated from it; the X wrappers by hand | Three places to keep in step |

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
| `E_NOT_SUPPORTED` | the operation does not apply to this object |
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

**Retired:** `SYS_FB_OP` (the display server draws, into memory objects);
`SYS_INPUT_READ` and `SYS_MOUSE_READ` (the input driver sends events to the
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
- The `.xfil` syntax above is a sketch; it is settled when the generator is
  written.
