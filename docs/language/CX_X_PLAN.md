# X: what comes next
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: PLAN.** The language as built is specified in
> `docs/x86_32/language/CX_X_CORE_LANG.md` (v0.3) and X Data in
> `docs/x86_32/language/CX_X_DATA.md`; both remain the reference until a new
> specification replaces them. This document is the order of work.

---

## 1. Already ahead of Zig, Rust and C

- **Address types** (`*user`, `*phys`, `*dma`): neither Rust nor Zig has them.
- **No ambient I/O:** a program reaches the world only through the syscall
  intrinsic.
- Deterministic builds; a self-hosting compiler (`os/xc`) held to the C#
  oracle.

## 2. For x86-64 (Phase 0)

- **`usize` and `isize`.** The spec has no pointer-sized integer; pointers are
  defined as 32-bit and lowering is defined against x86-32.
- **A target layer** in the C# compiler and in `xc`, and a **64-bit code
  generator** in both, held to each other by the differential tests.
- **Syscalls through `syscall`**, not `int 0x80`, on x86-64, and only through
  the system page's stubs (`kernel/CX_SYSCALLS_IPC.md` §4).
- **An intermediate representation.** The x86-32 backend emitted assembly
  straight from the tree (deleted in roadmap step 6d3c). A 64-bit backend is being written anyway, so it is
  written against a small typed IR, which becomes the target layer: x86-64
  now, ARM later, and a register allocator and simple optimisations on it
  instead of on assembly text.
- **The System V AMD64 calling convention**, so X and the C kernel call each
  other directly.
- **Debug information (DWARF)**, so GDB steps through X source and crash logs
  symbolise to X lines.
- Stage 5, the assembler and linker in X, is later (roadmap §3).

## 3. Security, next

- **Slices with bounds checks:** a pointer and a length, checked on access; a
  `*user` slice is validated once when it enters the kernel.
- **Overflow traps by default**, with wrapping available as explicit
  operators (Zig). Today overflow wraps silently. The 2026-10-01 security
  review found an integer wrap in the loader's signature offset parsing: this
  class of bug.
- **Non-null pointers by default:** `*T` is never null, `?*T` may be.
- **Error propagation** in the `try` / `errdefer` style on the existing sum
  types and `defer`.

## 4. Needed by planned work

- **Floats and SIMD:** compositor, audio mixing, spring animations, the layout
  engine. The rig has AVX and FMA.
- **Atomics with memory ordering:** threads and futex. Built in 1.5b1,
  sequentially consistent only: `__atomic_load(p)`, `__atomic_store(p, v)`,
  `__atomic_xchg(p, v)`, `__atomic_add(p, v)`, `__atomic_cas(p, e, n)` on a
  pointer to a 4- or 8-byte integer, each returning the old value (a store
  nothing). Intrinsics by name, as `__syscall` is
  (`devkit/CXEX.Lang/Sema/Intrinsics.cs`). Weaker orderings come before
  SMP, in the syntax for every target (§8, §9).
- **Thread-local storage:** built in 1.5c1 as one intrinsic, `__tls_base()`,
  a `usize` naming the calling thread's block (`as *T` to use it). It reads
  the word at `%fs:0`, which the block keeps pointing at itself (the x86-64
  ELF convention; the FX-8350 has no FSGSBASE to read FS_BASE directly).
  The kernel switches FS_BASE per thread and userland makes the block
  (1.5c2). No `thread_local` storage class: the library lays its block out
  as a record.
- **Effect attributes** `@isr`, `@nosleep`, `@noalloc`.
- **Namespaces and `pub`:** before packages.
- **Generics**, at compile time as Zig does, keeping lowering predictable.

## 5. The safety model

The hardening plan's open item: X needs a stated contract before its tests
mean anything. Proposed:

1. **No undefined behaviour outside `unsafe`.** Every operation either has a
   defined result or traps: overflow, division by zero, out-of-range shifts,
   out-of-bounds slice access, a failed checked conversion, a null `*T`. C's
   worst property, that a bug lets the compiler assume anything, is absent.
2. **`unsafe` blocks** hold what the compiler cannot check: raw pointer
   arithmetic, casts between pointer types, `*phys` and `*dma` access,
   reinterpreting bytes. `grep unsafe` is then the list of code to audit, and
   a review can ask why each one is there.
3. **Handles are linear.** A value of a handle type must be closed or moved
   exactly once; the compiler rejects a path that leaks it or uses it after
   moving it. This is Rust's ownership applied only to the resource CXOS
   actually hands out, without the rest of a borrow checker. Typed handles
   (`handle<Window>`) come from `.xfil`.
4. **Contracts:** `requires`, `ensures` and `invariant` on functions and
   structures. Checked at run time in debug builds; later, the input to a
   verifier that proves them (SPARK, Dafny), so X code can carry proofs
   (`safety/CX_SAFETY.md` §3.4).
5. **Checked conversions** (`try_as`), and `as` limited to conversions that
   cannot lose a value outside `unsafe`.

## 6. Tools

- **Test blocks in the language:** `test "name" { ... }` beside the code,
  compiled out of release builds, run by `cxk x test` (Zig's model).
- **A formatter** (`cxk x fmt`), so style is never a review comment.
- **A language server** from the C# front end: errors as you type,
  go-to-definition, hover types, for the VS Code extension (`editors/vscode`)
  and any other editor.
- **Diagnostics with codes and fixes:** every error has a stable code, the
  exact span, and a suggestion where one exists.
- **Random program testing.** A generator writes random valid X programs, and
  the C# compiler and `xc` must produce programs with the same output (Csmith's
  method), beyond the hand-written differential tests.

## 7. Known defects carried over

From `CX_X_CORE_LANG.md` §9: unchecked narrowing through `as`, `sizeof` on
type names, a cyclic alias reporting a half-expanded type, and mixed-sign
division following only the left operand.

## 8. Every target (Aurora, 2026-10-30)

X will run on x86-64, AArch64 and RV64, and perhaps a 32-bit CPU later
(roadmap §1). The ports are later; the rule is now. **A primitive is
defined by what it means, and lowered per target: no instruction names in
the language.** Each new primitive is proposed with its row in this table,
so a gap shows when the primitive is designed, not when a port starts.

| Primitive | x86-64 | AArch64 | RV64 | 32-bit notes |
|---|---|---|---|---|
| `__atomic_*(p, …, order)` | `mov`, or a `lock` instruction for seq_cst and read-modify-write | `ldar`/`stlr`, `ldaxr`/`stlxr` or LSE | AMOs with `aq`/`rl`, `lr`/`sc` | 8-byte: `cmpxchg8b` on i686, `ldrexd` on ARMv7, refused on RV32 |
| `__fence(order)` | `mfence` for seq_cst, nothing else at run time | `dmb ish` / `ishld` / `ishst` | `fence` with the matching sets | as 64-bit |
| `__spin_hint()` | `pause` | `yield` | `pause` (Zihintpause) | as 64-bit |
| `__tls_base()` | the word at `%fs:0` | `mrs TPIDR_EL0` | the `tp` register | `%gs:0` on i686, `TPIDRURO` on ARMv7 |
| `*mmio` access | one `mov` of the stated width | one access of the stated width; Device memory | one access of the stated width | as 64-bit |
| Division by zero | traps (`#DE`) | returns 0: the compiler emits the check | returns all ones: the compiler emits the check | as 64-bit |
| Overflow trap | `jo` after the operation | flag-setting form and a branch | no flags: a compare sequence | 64-bit values in two registers |

Orderings: `relaxed`, `acquire`, `release`, `acq_rel`, `seq_cst`, with
`seq_cst` the default, so 1.5b1's calls keep their meaning. They go in the
syntax now, although x86 lowers most of them to the same instruction:
x86's strong ordering hides a missing acquire or release from every test
run on it, and code written today should state its ordering instead of
being audited when a port starts.

The rest of the rule:

- **No target-sized types in the ABI.** `abi/kernel.xfil` uses `u64` for
  addresses and sizes, never `usize`, so every structure has one layout on
  every target. The ABI layout cross-check (`safety/CX_SAFETY.md` §7)
  holds it.
- **"Defined or trap" (§5) is what makes X portable.** The CPUs differ at
  the edges (division by zero, shift counts, misaligned access), and X
  defines each edge instead of inheriting the CPU's.
- **Byte order is stated.** Every target is little-endian, but parsers
  read through `std`'s `load_le32`, `load_be16` and the rest, so packet
  and on-disk code says which order it means.
- **DMA coherence is a library call**, `sync_for_device` and
  `sync_for_cpu` on the driver host's DMA buffer: a fence or nothing on
  x86, cache maintenance done by the kernel on that buffer's handle on
  ARM and on RISC-V parts that need it (Zicbom).
- **Vectors are portable types** (`vec4<f32>` and the like), lowered to
  SSE/AVX, NEON or RVV with a fixed length, sized for NEON's 128 bits. A
  CPU's own instructions sit behind a target check.
- **The IR is already shaped for this:** `cmp` yields a `bool`
  (`devkit/CXEX.Lang/IR/IR.md`), with no flags register to assume.

Testing a port before it exists, in three tiers:

1. **Compiler back ends under QEMU user mode** (`qemu-aarch64`,
   `qemu-riscv64`) in the CI container, held to the C# oracle program by
   program, as the x86-64 host programs are today.
2. **Real weak ordering:** an AArch64 phone (a spare Pixel 9) running
   statically linked X test programs over `adb`. QEMU on an x86 host
   inherits x86's ordering and cannot catch a missing barrier.
3. **The kernel port**, on QEMU `virt` first, then a board (a Raspberry Pi
   5 Compute Module is planned, with its serial port wired up).

Cheaper still, and possible now: the IR interpreter run as an abstract
target, with a 16-bit or 32-bit `usize`, every edge trapping, and
unsynchronised shared accesses reported, over the random X programs (§6).

## 9. Order of the language work (Aurora, 2026-10-30)

Compiler primitives only for what a library cannot express; everything
built on them lives in `std`. Each item is a change to the C# compiler,
its port in `os/xc`, and a Differential fact, so each is one step,
interleaved with the roadmap's (roadmap Phase 1, "Alongside").

1. **Layout:** `@packed`, `@align(N)`, `offsetof`, `alignof`. Today a field
   sits on at least 4 bytes, so a `u8` takes four, and no packet, on-disk
   or descriptor structure can be written exactly. An atomic on a packed
   field, and a misaligned field in `*mmio`, are refused (AArch64 and
   RISC-V fault on both). Feeds the ABI layout cross-check.
2. **Ordering, `__fence`, `__spin_hint`** (§8), before SMP.
3. **Slices and overflow traps** (§3), the two classes of bug a review has
   already found in this code.
4. **`try_as`**, and `as` limited to conversions that cannot lose a value
   (§5; the first defect in §7).
5. **Non-null pointers** (`*T`, `?*T`).
6. **`try` / `errdefer`** on the sum types and `defer`.
7. **`*mmio`**, an address type whose every access happens once, at its
   width, in program order. Before the first X driver (the Bochs display
   host, `kernel/CX_DRIVERS.md` §4).
8. **Contracts** (`requires`, `ensures`, `invariant`), checked at run time
   in debug builds.
9. **`@nosleep`, `@noalloc`**, for the audio and compositor paths.
10. **Floats, then portable vectors**, before the compositor's animations
    (roadmap Phase 2). First the kernel must save each thread's FPU and
    SSE state on a switch (`fxsave`/`xsave`); it does not yet, which is
    why `std/fixed.xfxn` exists.
11. **Generics**, at compile time.

Not taken, with the reason:

- **Interrupt-state intrinsics and `@isr`.** X runs at ring 3 (EL0, U-mode),
  where masking interrupts faults; a driver host gets an interrupt as a
  handle a thread waits on, so there is no interrupt context to restrict.
  Revisit only if X ever goes into the kernel.
- **DMA synchronisation in the compiler.** A library call (§8).
- **A `thread_local` storage class.** `__tls_base()` and a record laid out
  by the library (§4).
