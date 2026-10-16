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
- **An intermediate representation.** The x86-32 backend emits assembly
  straight from the tree. A 64-bit backend is being written anyway, so it is
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
- **Atomics with memory ordering:** threads and futex.
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
  go-to-definition, hover types, for the Studio and any editor.
- **Diagnostics with codes and fixes:** every error has a stable code, the
  exact span, and a suggestion where one exists.
- **Random program testing.** A generator writes random valid X programs, and
  the C# compiler and `xc` must produce programs with the same output (Csmith's
  method), beyond the hand-written differential tests.

## 7. Known defects carried over

From `CX_X_CORE_LANG.md` §9: unchecked narrowing through `as`, `sizeof` on
type names, a cyclic alias reporting a half-expanded type, and mixed-sign
division following only the left operand.
