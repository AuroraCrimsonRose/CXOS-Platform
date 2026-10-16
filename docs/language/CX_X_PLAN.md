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
- **Syscalls through `syscall`**, not `int 0x80`, on x86-64.
- **Stage 5, assembler and linker in X, multi-target from the start.**

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

## 5. Known defects carried over

From `CX_X_CORE_LANG.md` §9: unchecked narrowing through `as`, `sizeof` on
type names, a cyclic alias reporting a half-expanded type, and mixed-sign
division following only the left operand.
