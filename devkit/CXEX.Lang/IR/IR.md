<!-- SPDX-License-Identifier: MIT -->
<!-- SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems) -->
# The X typed IR

Roadmap step 6b2 (`docs/planning/CX_ROADMAP.md`, `docs/language/CX_X_PLAN.md`
§2). The C# compiler lowers the checked tree to this IR (`IrLowering.cs`),
`IrVerifier.cs` checks it, and `cxk ir <file.xfxn>` prints it. The x86-64
System V back end (`CodeGen/X64Emitter.cs`, step 6b3) reads it: `cxk compile
--target x86_64` lowers, verifies and emits ELF64. The x86-32 emitter still
works from the tree and is deleted with the 32-bit code (6d).

The lowering keeps the x86-32 emitter's meaning decision for decision, so the
two can be held to each other. The places it departs, deliberately, are listed
at the end.

## Types

| Type | Bits | Bytes in memory |
|---|---|---|
| `bool` | 1 (holds 0 or 1) | 1 |
| `i8` `i16` `i32` `i64` `i128` | 8 … 128 | 1 … 16 |
| `ptr` | `module pointer` × 8 | `module pointer` |

An integer type is a width, not a sign. `usize` and `isize` become the integer
as wide as `ptr` (`i32` while the pointer is 4 bytes). A plain enum is its
backing integer. A struct, a sum type and an array are never values: they are
bytes in memory, reached through a `ptr`.

## Operands

| Form | Meaning |
|---|---|
| `%n` | A virtual register. Assigned exactly once, by an instruction that runs before every use (dominates it). |
| a number | A constant of the type the instruction names, held reduced to that width (unsigned decimal). |
| `@name` | The address of a function, global or string: a `ptr`. |
| `$n` | The address of the function's stack slot `n`: a `ptr`. |

## Module

```
module pointer 4
string @str.0 = "hi\0a\00"                    bytes, NUL included; \xx is hex
global @g size 4 align 4 = { bytes 40 0 0 0 } initial bytes, low byte first
global @buf size 32768 align 4 zero           all zero
global @t size 8 align 4 section ".x" = { addr @f, zero 4 }
extern fn @puts(ptr s) -> i32                 declared, defined elsewhere
fn @f(i32 n %0, agg 12 p %1) -> agg 8 { ... }
```

A global's `addr @sym` item is a pointer-sized address. A global with a
`section` belongs in that section even when it is all zero.

## Functions

```
fn @sum(ptr a %0, i32 n %1) -> i32 {
  slot $0 4 align 4 ; a
b0:
  store ptr %0, $0
  ...
  ret i32 %14
}
```

A parameter is a value type, or `agg N`: an aggregate passed by value, which
arrives as the `ptr` to the function's own copy of its N bytes. The return is a
value type, `void`, or `agg N`. Every scalar parameter and every local lives in
a slot (`; name` says whose); temporaries' slots have no name. Blocks are
`b0` (the entry) onward; each ends in exactly one terminator.

## Instructions

| Instruction | Meaning |
|---|---|
| `%d = add/sub/mul T a, b` | Wrapping, modulo 2^bits. |
| `%d = sdiv/udiv T a, b` | Quotient, truncated toward zero. Division by zero traps, and so does `sdiv` of the most negative value by -1. |
| `%d = srem/urem T a, b` | Remainder; `srem` takes the dividend's sign (`-7 srem 2` is -1). Traps as the divide does. |
| `%d = and/or/xor T a, b` | Bitwise. |
| `%d = shl/lshr/ashr T a, b` | Shift `a` by `b` modulo the width of T; `ashr` fills with the sign. |
| `%d = neg T a` / `not T a` | `0 - a` / bitwise not. |
| `%d = cmp CC T a, b` | A `bool`. CC is `eq ne` on any type; `slt sle sgt sge` on integers; `ult ule ugt uge` on integers, `bool` and `ptr`. |
| `%d = trunc T a to U` | Keep the low bits (U narrower). |
| `%d = zext T a to U` / `sext` | Widen, filling with zero / the sign bit. `bool` widens only by `zext`. |
| `%d = ptrtoint ptr a to I` / `inttoptr I a to ptr` | Between `ptr` and the pointer-width integer; the bits are unchanged. |
| `%d = load T, p` | Read a T at `p`. |
| `store T v, p` | Write v at `p`, the type's own number of bytes. |
| `copy dst, src, N` | Copy N bytes. Ranges that overlap without being equal leave `dst` unspecified. |
| `zero dst, N` | Clear N bytes. |
| `%d = ptradd p, off` | `p + off`, off a pointer-width integer. |
| `%d = call f(T a, agg N p, ...) -> T` | Call `f` (`@name` or a `ptr` register). An `agg N p` argument passes a copy of the N bytes at `p`. |
| `call f(...) -> agg N dst` | The aggregate result is written to the N bytes at `dst`. |
| `call f(...)` | No result. |
| `%d = syscall T (I n, I a1, ...)` | System call `n` with up to five pointer-width words; the target decides the mechanism (the system page on CXK). |

## Terminators

| Terminator | Meaning |
|---|---|
| `br bN` | Jump. |
| `brif c, bT, bF` | `c` is a `bool`. |
| `switch T v, bD [k: bK, ...]` | Jump to the case equal to `v` (each `k` once), else to `bD`. |
| `ret` / `ret T v` | Return nothing / a value. |
| `ret agg N src` | Return the N bytes at `src`. |

## What the lowering decides

These are the language's rules, written down once here so a back end never
re-derives them from the tree.

- **Integers up to 32 bits are worked on at 32**, as in C. `+ - * & | ^` give
  the same low bits at any width, so they are done at the result's type
  directly. Divide, remainder, shifts and mixed-type comparisons widen each
  operand to `i32` by its own sign, operate, and cut back to the result type.
- **Divide, remainder and `>>` take the left operand's sign.**
- **A comparison of two different types** widens both to 32 bits by their own
  sign and is unsigned if either is an unsigned 32-bit integer or an address
  (C's rule). Two values of the same type compare at that type with its sign.
- **Past 32 bits** both operands take the wider type (a shift's count takes
  the left's), and that type's sign decides the operation.
- **Conversions**, implicit or `as`: an integer narrows by truncation and
  widens by its own sign; a pointer goes through the pointer-width integer;
  `as bool` is `!= 0`.
- **Evaluation order is the emitter's:** a binary operator's right operand
  before its left, an assignment's value before its target's address, call
  arguments last to first and then an indirect callee, `__syscall` arguments
  first to last, a record literal's fields as written, an index's base before
  the index.
- **`return x` takes the value before the defers run.** Each exit (end of a
  block, `return`, `break`, `continue`) runs its own copy of the defers it
  leaves, innermost first.
- **Layout is the target's** (`XTarget`, `SemaContext.AlignOf`): a field is
  aligned on its size clamped to 4 and the target's largest alignment (4 on
  x86-32, 16 on x86-64), a struct on its widest field, and its size rounds up
  to that. On x86-32 every field is on 4, as X has always been; on x86-64 a
  `u64` or a pointer is on 8 and an `i128` on 16, as System V lays out C.
  `module pointer` gives the pointer width; `usize` and `isize` follow it.
- **A sum type** is a `u32` tag (the variant's index) and the payload at the
  widest alignment of its variants' fields, at least 4; a variant value zeroes
  the payload before writing its fields.
- **A switch** evaluates its subject once (a sum type's by address); a binding
  is the address of the payload.
- **Falling off the end** of a function returns zero (an aggregate: its
  return slot as it stands).

## Where the emitter is not followed

- `as bool` is `!= 0`. The emitter left the whole register, so the "bool"
  could hold 300 until a store cut it to 44.
- A 64-bit operand in a 128-bit operation widens by its sign. The emitter read
  16 bytes from the 8-byte value.
- Falling off the end returns zero, not whatever `eax` held.
- A `__syscall` argument wider than a pointer is refused (the emitter passed
  its address); the ABI splits wide values into register pairs itself.

## The x86-64 back end

`X64Emitter.cs` spills everything: each value has a frame home of 8 bytes (16
for `i128`), and an operation loads, works at its own width and stores. A
value narrower than its home keeps unspecified upper bits. Calls follow System
V AMD64: six integer registers, an `i128` in a register pair or on the stack
on 16, an aggregate of up to 16 bytes in registers and a larger one in memory,
a returned aggregate over 16 bytes through a hidden pointer. `i128` divide and
remainder are the helpers `__cx_udivmod128` and `__cx_sdivmod128`; a zero
divisor faults as the 32-bit one does. `__syscall` is `call *%rax` into the
CXK syscall page (rax = base + n·16, arguments in rdi, rsi, rdx, r10, r8);
with `--no-prelude`, on a Linux host, it is the `syscall` instruction.

Not yet right, and known:

- An `i8` or `i16` argument to C is not sign- or zero-extended (the IR is
  signless); clang's callees assume it is.
- A `u8` field is on 4 in X and on 1 in C, so a struct with one does not match
  C's layout.
- An aggregate aligned on 16 passed on the stack is aligned on only 8.
- `_start`'s stack alignment and the syscall-page path run only when the
  executive starts on x86-64 (6d); the tests use `--no-prelude`.
