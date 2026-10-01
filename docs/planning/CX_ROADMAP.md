# CXOS Roadmap
### CX Design Spec — Aurora Tejeda / CATX SYSTEMS LLC

> **Status: DIRECTION, not schedule.** This records where CXOS is going, the
> order it should get there, and the reasoning behind each call, so decisions
> survive longer than the conversation they were made in. Nothing here has a
> date. Items move between phases when the reasoning changes, and the reasoning
> is written next to each item so that when it does change, it is obvious which
> calls it affects.

---

## 0. The rule that sorts everything else

The list of things CXOS will eventually need is roughly thirty major
subsystems, several of them individually larger than everything CXOS is today.
That is not a reason to shrink it. It is a reason to have a rule for sorting it,
because the failure mode for a project this size is not a lack of ambition — it
is spending a year on something that should have been borrowed and losing the
thread on the thing that only this project can build.

| | Meaning | Examples |
|---|---|---|
| **Build** | It *is* the project | kernel, X and its dialects, CXFS, the capability model, GUI toolkit, package manager, task manager |
| **Port** | Needed, but not ours to invent | zlib, FAT32, codecs, SSH, an HTTP server, a SQL server |
| **Embed** | Someone else's engine inside our shell | a web browser — **never write a browser engine** |
| **Skip until forced** | Real, but not yet worth its cost | NTFS, CUDA, antivirus definitions, a native NVIDIA driver |

A second rule applies across all four: **do the cheap things that are painful
to retrofit; defer the expensive things that are not.** This is why the 64-bit
offset went into `SYS_MEM_OP` before anything could use it, and it is the test
that decides several items below.

---

## 1. Phases

### A — Self-hosting *(now)*

**X compiles X.** Every other language on this page is a C# project maintained
forever until this is true, and an OS that cannot build its own compiler is
borrowing its toolchain. This is the single highest-leverage item on the page.

It is also the gate for writing kernel code in X (§5): a compiler exercises far
more of the language than a kernel does — recursion, data structures, strings,
dynamic allocation, error paths — so if X cannot build its own compiler it has
no business building page tables.

Route stages 1–3 in `CX_X_CORE_LANG.md` §10 are done: file I/O, memory,
growable buffers (`std/buf.xfxn`), sum types and `switch`, along with
character literals, compound assignment, struct and array initializers, and
integer conversions that need `as` wherever a value could change. The integer
gaps are closed too: wide division, 128-bit literals, variable shifts, wide
and struct values by value (C-compatible), literal suffixes. **Stage 4 — the X
compiler written in X — is done, in `os/xc`:** lexer, parser, type checker and
code generator, each held to the C# compiler by a differential test; `xc`
compiles itself to a fixed point, and compiles itself on CXK, byte for byte.
Stage 5 - an assembler and linker, so CXK needs no cross toolchain - is next.

**Hardening runs alongside, and its Phase 1 comes first.** The 2026-10-01
security and engineering reviews are answered in `HARDENING_PLAN.md`:

- **Phase 0, tooling.** Tests move to the DevKit's xUnit project
  `CXEX.Tests`. Platform scripts give way to `cxk` commands: `cxk os build`,
  `cxk run`, `cxk uefi build`.
- **Phase 1, the executable boundary.** The CXEX loader validates the whole
  image before mapping anything, and never maps outside user space. Its
  arithmetic is overflow-safe, the signature covers every byte it reads, and
  W^X is enforced.

Both come before stage 5. A self-hosting system that loads untrusted images
unsafely has gained a compiler and kept the hole. Phases 2 and 3 (IPC
resources, subsystem contracts) interleave with stage 5 and phase B.

Alongside, the language features that are cheap now and expensive later:

- **Distinct address types** — *done*, see `CX_X_CORE_LANG.md` §2.2.
  `*user`, `*phys`, `*dma`, with no implicit conversion (plain `*T` covers
  what `*kernel` would have meant). The highest-value language item: it is the only one that
  prevents a class of *security* bug (dereferencing an unvalidated user pointer,
  handing a virtual address to DMA). No codegen change — all pointers stay the
  same width — so it is type-checker work only. Every conversion must be a
  named, greppable operation.
- **`defer`** — *done*, see `CX_X_CORE_LANG.md` §2.3. Scope-exit cleanup,
  Zig-style (lexical, no closure, no heap).
- **Attribute syntax** — *done*, see `CX_X_CORE_LANG.md` §2.4, with
  `@section` as the first attribute. X had none. It is the prerequisite for ISR contexts
  and declarative driver tables (§5), so add the syntax early even if the
  attributes come later.

### B — Minimum viable OS

The "GEOS-style" bar: the smallest system that genuinely meets spec and that
someone could use. Made explicit so it cannot quietly grow.

- **X Data** (§2) — first, because six things already need it
- **FAT32** read/write — interop with every other machine is non-negotiable
- **Software rasterizer + compositor** — the unblocked path to a real GUI
- **X Shell** (§2) and a terminal
- **Task manager** (§4)
- **Basic apps** — clock, calendar, text editor, file browser
- **Networking completion** — DNS, NTP, UDP then TCP

### C — ARM

Next architecture after x86-32 is ARM, targeting the Raspberry Pi 3B+, then
x86-64. This is where the portability discipline pays off — see
`CX_ABI.md` and the rule that the OS must not hold architecture facts.

**Bring up QEMU `virt` first, then the Pi.** The 3B+ boot path is unusual — the
VideoCore GPU starts first, firmware blobs, a mailbox interface — and it is
ARMv8 commonly run in 32-bit mode. QEMU `virt` is a clean, documented ARM
machine. Porting the architecture there and then moving to the Pi costs the
same total effort and far less time debugging the wrong layer.

### D — Ecosystem

- **Package manager** with signed, layered updates (§3)
- **Package repository** on CATX infrastructure
- **Key distribution service** — globally recognised developer keys, OS update
  keys, revocation
- In-place updates

### After D

Everything else: GPU acceleration, a SQL server, a web server, the browser,
remote desktop, antivirus, AI integration, identity, Bluetooth, Wi-Fi, audio,
codecs, compatibility layers. Slotting any of these earlier is how a five-year
project becomes a twenty-year one.

---

## 2. The languages

One semantic core, several surfaces — see `CX_X_CORE_LANG.md`.

### A language's name is not a domain

**The only domain is the letter directly after `x` in a file extension.** A
language is always called by its full name, and its initials are never written
as though they were a code — because a two-letter nickname reads exactly like
`x` + a domain letter, and several of them would collide with real ones:

| Nickname | Would read as | Why that is wrong |
|---|---|---|
| "XS" | x · **S** | `S` is the System tier (`.xsex`) |
| "XR" | x · **R** | `R` is reserved for the Runtime domain (`CX_EXTENSION_SYSTEM.md` §6) |
| "XN" | x · **N** | was the reserved Network domain; network files are now `.xsnt` / `.xunt`, but the nickname would still read as a code |
| "XD", "XV", "XH" | x · **D**, **V**, **H** | none of these is a domain, and writing them as codes suggests they are |
| "XGL" | x · **G** | graphics source lives in **F**, not a G domain |

So the name is written in full — **X Data**, not "XD" — and the two letters at
the end of an extension (`.xf`**`xd`**) are an **object-type** code inside the
domain, which is a different thing from a domain.

### Every language's source is in `F`

`.xfxn` has always been X Native *source*, in the **F** (format) domain. Every X
language's source follows it — the domain is F, and the last two letters name
the object type:

| Language | Source | Status |
|---|---|---|
| X Native | `.xfxn` | working |
| X Runtime | `.xfxr` | planned (dialect of X Native) |
| X Hybrid | `.xfxh` | planned (dialect of X Native) |
| X Data | `.xfxd` | **next** |
| X Visual | `.xfxv` | after the layout engine |
| X Graphics Language | `.xfgl` | after the software rasterizer |
| X Shell | `.xssh` / `.xush` | phase B — see below |

The dialects are distinguished by their **source** extension only. Compiled, they
are the same kind of artifact as X Native — there is no compiled-X-Runtime or
compiled-X-Hybrid extension, because an executable is named for whose it is,
not for what produced it (`docs/devkit/CX_DEVKIT_DESIGN.md`).

> **Correction.** An earlier draft of this page said graphics should not be
> `XFGL` because "a shader language is not a format." That was wrong: `F` is
> where every X language's source already lives, `.xfxn` included. X Graphics
> Language source is `.xfgl`.

### The one exception: shell scripts take a tier

**X Shell** scripts are the exception to "source is in F", deliberately. A script
is run, and it runs with authority — exactly the property the ownership tiers
exist to express for executables. So a script's domain says *whose* it is, as
an executable's does: `.xssh` is a System-owned script, `.xush` a User-owned
one. It is still one language; a signed fix shipped by CATX is a System-tier
script, not a different language.

### Serialization is not called "XS"

`S` is the System ownership tier, and a language nicknamed "XS" would put two
meanings of `S` into a taxonomy whose whole point is that the letter says whose
a file is — the mistake that retired `.xcex`. It is **X Data**, source `.xfxd`.

### X Data *(started — reader built, see `CX_X_DATA.md`)*

The v0 reader is `os/std/xdata.xfxn`. The first consumer should be the `.xosv`
service descriptors, whose hand-parsed `key=value` format cannot express an
argument containing a space.

A serialization format in the spirit of RON or TOML: typed, commentable, and
pleasant to write by hand. It is the sleeper item on this page — small, and
already needed by:

- `.xosv` service descriptors (hand-parsed `key=value` today)
- package manifests
- system / user / workspace configuration (§3)
- secrets-vault metadata
- declarative driver tables (§5)

Two properties that must be designed in, not added:

- **Schema-checkable.** This is what delivers "strict types like Postgres."
- **Round-trip preserving.** A tool that rewrites a config file must keep its
  comments and its ordering. This is the single reason people hate JSON for
  configuration, it is cheap on day one, and it is impossible to retrofit.

Writing its parser in X is also exactly the self-hosting practice phase A needs.

### X Visual *(after the layout engine)*

One language for structure, style and behaviour — drawing on HTML, CSS, XAML,
SCSS, TypeScript and React — indentation-based, compiling straight to the
native layout engine with no XML tag bloat. **Single-file components** — no
forced split between markup, style and logic — are the real differentiator,
and that is a design decision that costs nothing to honour.

**Build the layout engine first, as a library with an API.** The syntax is a
week; the layout engine is a year. HTML and XAML are not verbose because nobody
thought of indentation — they are complex because of the cascade, the box
model, flex and grid solving, text shaping and reactive invalidation. Write
screens against the engine in raw X Native, find what hurts, then design X Visual
to remove exactly that. Designing the language against an engine that does not exist gets
the abstractions wrong.

Indentation-based syntax reads better and is worse for generated content and
for diffs of deeply nested trees. Accepted knowingly.

When separate data is wanted, it goes in X Data.

### X Shell

The diagnosis is right for bash: everything is a string. **PowerShell is not a
counter-example** — it pipes .NET objects. Its real problems are verbosity,
startup cost, and an object model that is .NET's rather than the OS's.

X Shell pipes **X values**, typed and checked by X's type checker. PowerShell
had to borrow .NET's type system; X Shell pipes the OS's own native types. That is a real
advantage and most of it already exists.

Target uses: interactive shell, automation, deployment.

### X Graphics Language

Source `.xfgl`. See §6. In short: a shader language compiling to **SPIR-V**, which is bounded,
well specified, and can be validated with `spirv-val` without any GPU at all.

---

## 3. Data: one store, not three

Configuration, the secrets vault and the package database are one system.

**Do not write SQL.** A SQL engine — parser, planner, optimiser, executor — is
a decade of work, and the goal is explicitly to escape SQL's quirks. What CXOS
needs is a **typed embedded store**: a B-tree, a schema, transactions, crash
recovery, and a **native X API** in place of a query language. Strict types get
*easier* without SQL, not harder.

Working name: **XFDB** (`.xfdb`, X Format Database).

| Use | What it is |
|---|---|
| Configuration | typed records, scoped system / user / workspace |
| Secrets vault | the same store, encrypted values, capability-gated reads |
| Package database | installed set, versions, file ownership |
| Key Vault | already exists as `/System/KeyVault`; becomes a table |

### Configuration is not a registry

The lesson from the Windows registry is **no single point of failure**. The
design that delivers it:

- a **separate file per scope** (system, user, workspace)
- **schema-validated** on read
- a **defined merge order**, workspace over user over system
- a **corrupted scope degrades to defaults** instead of breaking boot

That last rule is an architectural property. Write it down now; it cannot be
bolted on.

### The hard part

**Durability** — fsync semantics, write-ahead logging, crash consistency. That
is where SQLite's twenty years went. Test it by killing the VM mid-write, and do
not believe it works until that has been done a few hundred times.

Full SQL servers (a PostgreSQL or MariaDB equivalent) are **port, not build**,
and much later than it seems.

---

## 4. System components

### Updates as signed layers *(design early)*

Each update is a **layer**; the running system is a stack of them; an update is
*verify and stack*, a rollback is *unstack*. This gives a structurally immutable,
restorable system in the manner of OSTree or A/B partitioning.

It is cheap when designed into the package manager from the start and brutal to
retrofit — and it fits what already exists: every layer is a signed artifact,
so the trust model in `CX_EXTENSION_SYSTEM.md` does the hard half.

### Task manager

Windows-task-manager style: tabbed, optionally always on top.

1. **Processes** — running processes, their grants and memory
2. **Performance** — CPU, memory, temperatures, clock speeds, fan speeds
3. **Services** — the supervisor's view of `.xosv` services
4. **System** — components and speeds; thorough, but not so dense it is hard to read

### Filesystems

**FAT32 read/write first, then stop for a long while.** Universal, simple, and
how files move in and out. EXT4's journaling is substantially harder; NTFS is
realistically read-only. CXFS stays native.

### Security engine

Sandboxing, isolation, quarantine, built on standard malicious-software
definitions. The sandbox half largely exists already — capabilities, attenuation,
per-process quotas, signed execution, with the loader hardening of
`HARDENING_PLAN.md` closing the gaps the 2026-10-01 review found. Definitions need a *pipeline*, not just
code, which is why they are "skip until forced."

### Remote access

**Implement real SSH.** The value is connecting *from* machines that already
exist, so compatibility is the entire feature; a new protocol that nothing else
speaks is one only CXOS can use. Remote *display* is separate and later,
possibly over the same transport.

### Compatibility with Linux/Unix and NT

The working names were "XINX" and "XNT". They are written out here, because
"XNT" in particular now reads as `nt`, the network object type. Linux/Unix and
NT compatibility. **Prefer compiling their source to `.xuex`
over emulating their ABI.** An ABI layer chases syscall semantics forever;
compiling from source is tractable and fits the signing model. Pick one, much
later.

### Audio and video

An audio subsystem (Realtek codecs on AMD boards, plus whatever QEMU exposes),
an audio control engine, codecs, a media player. **Node graphs** as the internal
model for both audio and video are a good fit and worth designing toward.
Codecs are a patent minefield as well as large — port.

### Identity

YubiKey and hardware tokens, credential stores. Depends on an account model
that does not exist yet, deliberately — see "accounts" below.

### Accounts

Deferred on purpose. An account model needs foresight that should come from
having an OS worth protecting. Until then, an unsigned or unknown-signer
`.xuex` is refused outright — safe, correct, and strict.

---

## 5. The kernel and X

**The kernel is 71 C files and 0 X files.** Moving it to X is a *direction,
not a project*: never schedule the rewrite.

C does not limit a kernel — it is the most permissive systems language there
is. What X would add is **limits**, deliberately chosen, in the places C cannot
provide them: deterministic bitfield layout, address types instead of `void*`,
an effect system, no strict-aliasing traps, and real metaprogramming instead of
macros.

The cost: **the compiler becomes the most safety-critical thing CXOS owns.** A
codegen bug in userland is a wrong number; in `paging_map` it is a corrupted
address space, and it is ambiguous whether the compiler or the kernel is at
fault. This is not hypothetical — see the mixed-width bug in `CX_X_CORE_LANG.md`
§2.1, which silently did 32-bit arithmetic on a 64-bit value's *address*.

**Order by crash containment, not by importance:**

1. userland — done
2. OS services
3. **drivers** — the proving ground; isolated, numerous, and exactly where
   MMIO layouts and address types pay off
4. the core kernel — last, and parts of it stay assembly regardless

**No rewrite.** X writes new kernel code; C keeps what works. Both produce ELF
objects and link together. The C kernel staying buildable is also a known-good
reference to bisect against when X-compiled code misbehaves.

### Kernel-language features, assessed

| Feature | Verdict |
|---|---|
| User / kernel / phys / virt / DMA pointer types | **yes, phase A** — one feature, the best one |
| `defer` | **yes, phase A** |
| Deterministic bitfields and MMIO layouts | **yes** — genuinely better than C, whose bitfield layout is implementation-defined |
| Declarative hardware matching → ELF section | **yes**, cheap; pays off once drivers are in X |
| Compiler-enforced ISR contexts | **yes, generalised** — an effect system (`@isr`, `@nosleep`, `@noalloc`) beats a one-off |
| *Automatic* memory barriers | **no** — barrier placement is architecture-specific and a design decision; automatic insertion puts in the wrong ones. Take the other half instead: MMIO access the compiler never reorders, coalesces or elides, plus explicit barrier intrinsics |
| Built-in polling with timeouts | **no** — a library function |
| Native spinlocks and mutexes | **no as language features.** On a uniprocessor kernel a spinlock deadlocks against itself; interrupt masking is what protects a critical section. What a language *must* supply is **atomics**; the lock is a library type on top. Binding data to its lock (Rust's model) is worth wanting and is hard — later |
| Syscall table instead of interrupts | **already exists** — `syscall_dispatch` is the table. `int 0x80` is the *transition*; the alternative is `sysenter`/`syscall`, a performance change, x86-only, behind the kernel boundary |
| Fastcall for hardware registers | **general codegen work, low priority** — a register calling convention speeds up everything, and is an ABI break for hand-written assembly |

---

## 6. Graphics and GPU

The most ambitious area, and the one where the bottleneck is most often
misplaced.

**The hard part is not the shader compiler. It is the kernel side**: PCIe BAR
mapping, GPU memory management, command submission rings, interrupts, firmware
loading, and an IOMMU. None of that comes free from borrowing a driver's
compiler.

### Order

1. **X Graphics Language → SPIR-V.** Bounded, specified, verifiable without hardware.
2. **Software rasterizer.** Every GUI item — compositor, windows, task manager,
   apps — works on it, today, with no vendor cooperation.
3. **virtio-gpu (Virgl / Venus) under QEMU.** Real 3D acceleration without
   writing a hardware driver.
4. **Native NVIDIA** — someday, not a phase.

### On the NVK / NAK / NVPTX route

The sketch — NVPTX through LLVM, then SASS through NAK, with a thin driver as a
front end and memory manager — is a real plan, not a fantasy. Things to know
before starting it:

- **NVK exists because NVIDIA changed.** It is built on the GSP firmware and
  open kernel modules NVIDIA began shipping around 2022, by full-time Mesa
  developers. It is not clean-room reverse engineering that can be shortcut.
- **The Rust dependency is circular.** NVK and NAK are Rust. Teaching Rust to
  target CXOS and writing a `core`/`std` for it is a larger project than the
  driver front end it would save — for an OS that cannot yet compile its own
  compiler.
- **Licensing.** Mesa (NVK, NAK) is MIT and LLVM is Apache 2.0 with the LLVM
  exception — both compatible with a proprietary kernel. **The kernel-side
  `drm/nouveau` is GPL**, which is a real constraint under CXOS's licensing
  model. Know it before reading that code.
- **Requires first:** SMP, PAE or 64-bit, an IOMMU, and PCIe BAR management.

### On gaming

Wanting GPU access for the GUI and for compute is entirely legitimate. Gamers,
though, need *games* — Win32, DirectX, anti-cheat — which is a vastly larger
lift than a GPU driver, and the thing Proton needed a decade and Valve's
funding to approach. Treating CUDA as the gate for gamers sets a target that
makes every other decision feel late.

---

## 7. AI integration

**Opt-in**, installed online, never on by default.

**Not at the low level.** A local inference runtime is a library plus a compute
driver, not a kernel feature, and placing it low would grant exactly the
unbounded access that should be avoided. Its home is a **service with
capability grants** — which CXOS already expresses precisely: an agent holds
`GRANT_DISK` or it does not, `GRANT_NET` or it does not, and anyone can see which.
Whether the agent arrives over MCP or runs locally, the grant model is the same.
That is a better AI permission story than any mainstream OS has, and it is
already built.

---

## 8. Telemetry

Anonymous: machine specifications, what gets used and how much, what crashes,
and what the security engine flags — for trends, never for tracking.

**Opt-in, with a clear first-run explanation.** Opt-out telemetry is a
reputational landmine for an independent OS, it is the thing people will write
about, and once an EU user installs it there are GDPR obligations. Opt-in with
an honest explanation gets more trustworthy data than opt-out gets goodwill.

---

## 9. Hardware and drivers — outstanding

- **IOMMU** — required before any serious DMA device, and before GPU work
- **NX bit** — requires PAE on x86-32; PAE is also three of the four levels of
  x86-64 paging, so it is not throwaway work. Take NX, defer >4 GB physical.
  See `CX_ABI.md` for why `W|X` is refused today even though it cannot be enforced.
- **SMP** — CXK is uniprocessor today
- **Bluetooth, Wi-Fi** — after D
- **Audio** — Realtek HDA codecs, QEMU's audio devices

---

## 9a. Kernel stacks, and boot configuration

Prompted by two Linux proposals — dynamic kernel stacks that grow on demand, and
a boot-time parameter for the stack size so Android can use smaller stacks.
Neither pays off for CXK yet, but both rest on something CXK is missing, and the
order in which to add things matters more than either proposal.

**Where CXK stood.** Kernel stacks were `kmalloc(THREAD_STACK)` (`sched.c`) —
8 KB of ordinary heap with no guard page below it, and the double-fault
handler ran on the same stack as everything else. So a kernel stack overflow
did not fault at all: it silently overwrote whatever heap object sat below
the stack. That is the worst class of bug there is — the damage appears later,
somewhere unrelated, with nothing pointing back at the cause. It had already
happened once: CXFS's 4 KB staging buffers were locals, and a file syscall ran
8.6 KB deep on an 8 KB stack (see the note in `cxfs.c`).

In order:

1. **Guard pages and a double-fault task gate — done.** Every kernel stack
   (`memman/kstack.c`) sits at the top of its own 64 KB slot in a reserved
   region at `0xCF000000`; the 56 KB below it is never mapped, so an overflow
   is a page fault instead of corruption, and a single large frame cannot jump
   the gap the way it can jump a one-page guard. The double fault goes through
   a task gate to a second TSS with its own stack (`gdt.c`, `idt.c`) — the
   32-bit equivalent of x86-64's IST, which it maps onto directly. The page
   fault from an overflow cannot be delivered on the stack that just ran out,
   so it escalates to a double fault; with a stack of its own that is a panic
   naming the thread, where before it was a triple fault and a silent reset.
   Both halves are proven, not assumed: `CXK_KTEST_STACK_OVERFLOW=1`
   (`tools/cmake/cxk_flags.cmake`) overflows a stack on purpose and the boot
   ends in "Kernel stack overflow: thread 1 (overflow)"; the same build with
   vector 8 put back on an interrupt gate triple-faults. Thread 0 is guarded
   too: kmain leaves the `.bss` boot stack for a guarded one right after
   `kstack_init`, and thread 0 now has an esp0 stack of its own (before, its
   trips to ring 3 used whichever esp0 the previous thread had left in the
   TSS). `CXK_KTEST_STACK_OVERFLOW=2` overflows thread 0 and the panic names
   "main".

   Guarding the stacks immediately exposed a scheduler race that had been
   there all along: `yield()` and `thread_exit()` switched with interrupts on,
   and a timer tick between `current = next` and the switch saved the exiting
   thread's stack pointer as the next thread's context. The exiting stack was
   then freed under it. On heap stacks that read back correctly by luck; on
   guarded stacks it faults. Both now switch with interrupts off, and process
   threads are created held (`thread_create_process`) until their records,
   esp0 stack and uid are in place. With the race window artificially widened,
   the old code panicked on 6 of 6 boots and the fixed code on none.

2. **Boot configuration, in X Data — done.** `/System/Config/kernel.xkco`
   (`os/config/kernel.xkco`, `kernel/kconfig.c`), read as soon as the system
   volume is mounted and before anything creates a thread. Typed values,
   range-checked, and the whole file is checked before any of it is applied: a
   typo, an unknown key or a value out of range refuses the entire file, the
   log names the line, and the kernel boots on built-in values. The build runs
   `cxk check-xdata` on it first, so most mistakes never reach a disk.

   The kernel reads it with **the same X Data reader the supervisor uses** —
   `os/std/xdata.xfxn`, compiled with `cxk compile --object` and linked into
   the kernel like any C object (`kernel/lib/format/xdata.h`). That is the
   kernel's first code written in X, and it works because X's calling
   convention is already cdecl. It also means there is no third reader to keep
   in agreement with the other two.

3. **The limits are configuration — done.** `kernel_stack_kib` (8–32),
   `max_threads` (4–32) and `memory_quota_mib` (1–64). `MAX_THREADS` is now
   the tables' capacity (64); the limit in force is a runtime value beneath it.
   Tunable stack size came after step 1 on purpose: a stack set too small now
   stops the machine with a panic naming the thread, instead of corrupting
   memory with no trace.

4. **Dynamic stack growth — after SMP and real threading.** Mapping stack pages
   on demand saves memory in proportion to the number of threads. At eight
   threads of 8 KB that is nothing; it becomes worth its considerable complexity
   (the fault is taken on the exhausted stack itself, and the handler cannot
   sleep) only when thread counts reach the hundreds.

---

## 10. Open questions

- Literal suffixes vs. bidirectional type inference, to let `1 << 100` be
  written at 128 bits
- Whether X Data's schema language is X Data itself or a separate form
- A memory budget charged to a whole process *subtree*, rather than the current
  per-process quota — today a process that spawns without end still exhausts RAM
- Whether XFDB's API is a library only, or eventually grows a query surface
