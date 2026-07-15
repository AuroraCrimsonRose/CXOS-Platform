# X — Core Language v0.1 (Frozen Spec)

**Status:** FROZEN CONTRACT (core v0.1). X is the systems core of the X family.
XR (runtime) and XH (hybrid) are **front-end dialects that desugar to X core** —
they are *not* separate compilers and have no independent backend. There is one
semantic core and one CXEX backend, so the dialects cannot drift. This document
freezes the core; dialects are specified later against this target.

Design rule, same as the ABI: freeze the smallest useful core now; add features
on demand. No separate IR yet — **X core's typed AST is the lowering target**
that XR/XH reduce to and that the backend consumes. An explicit IR is added only
if/when optimization needs it.

---

## 1. Principles

- **Freestanding, no runtime.** X core has no GC, no allocator, no exceptions,
  no hidden calls. A compiled X program is a flat CXEX image, like the C apps today.
- **Capability-native: effects only via syscall intrinsics.** X has *no ambient
  I/O*. The only way to affect the world is `__syscall`, which lowers to `int 0x80`
  against the frozen ABI (`abi/cxk_abi.h`). So an X program is bounded by exactly
  the capability surface the kernel grants it — sandboxing falls out of the
  language, not a checker bolted on. XR's "runtime" is just X code that calls
  executive services through these same intrinsics.
- **Predictable lowering.** Every construct has an obvious, fixed mapping to
  x86-32. No surprises, no implicit allocation, no implicit copies beyond what's
  written. This keeps X auditable — fitting an OS you sign and trust.

---

## 2. Types (v0.1)

| Category | Types |
|----------|-------|
| Signed   | `i8 i16 i32` |
| Unsigned | `u8 u16 u32` |
| Boolean  | `bool` (1 byte; `true`/`false`) |
| Pointer  | `*T` (32-bit) |
| Void     | `void` (return type only) |
| Aggregate| `struct` (named, by-value or via pointer) |
| Array    | `[N]T` (fixed size) |

No `i64`/`u64` arithmetic in v0.1 (mirrors the kernel's no-64-bit-divide rule);
64-bit values are passed as structs/pointers, as in the ABI. No floats in v0.1.
Pointer arithmetic is explicit and scaled by `sizeof(T)`. No implicit conversions
except literal→sized-int where it fits; everything else needs a cast `as`.

---

## 3. Declarations

```
// function
fn name(p0: T0, p1: T1) -> R { ... }      // -> R optional; default void

// struct
struct Name { field0: T0, field1: T1 }

// global (must have a constant initializer or be zero)
global g: i32 = 0;
const  K: u32 = 0x1000;                    // compile-time constant

// extern (resolved at link, e.g. another X/asm symbol)
extern fn other(x: i32) -> i32;
```

- Entry point is `fn _start() -> void` (matches the CXEX entry convention).
- Functions use cdecl-compatible calling convention (args on stack or the agreed
  register set), so X, asm, and the C bootstrap interoperate during the transition.

---

## 4. Statements & expressions

```
let x: i32 = expr;        // local; type optional if inferable from initializer
x = expr;                 // assignment
if (cond) { } else { }
while (cond) { }
return expr;              // or bare `return;`
expr;                     // expression statement (e.g. a call)
```

Expressions: integer/bool literals, identifiers, calls `f(a, b)`, member `s.field`,
deref `*p`, address-of `&x`, index `a[i]`, the usual arithmetic/comparison/logical
operators, and `expr as T` casts. Operator semantics are C-like with **defined**
overflow (wraps, two's complement) — no UB; X is meant to be predictable.

---

## 5. The syscall intrinsic (the only effect)

```
// the primitive: number + up to 5 args, returns the raw eax result
__syscall(n: u32, a1: u32, a2: u32, a3: u32, a4: u32, a5: u32) -> i32;
```

Lowers directly to the ABI calling convention (`eax=n`, `ebx..edi=a1..a5`,
`int 0x80`, result in `eax`). Unused args pass 0. From this, typed wrappers are
generated from `abi/cxk_abi.h` so X code reads naturally and stays in lockstep
with the kernel:

```
fn console_write(buf: *u8, len: u32) -> i32 { return __syscall(SYS_CONSOLE_WRITE, buf as u32, len, 0,0,0); }
fn ipc_call(args: *IpcCallArgs)      -> i32 { return __syscall(SYS_IPC_CALL, args as u32, 0,0,0,0); }
fn exit(code: i32)                   -> void {        __syscall(SYS_EXIT, code as u32, 0,0,0,0); }
```

The `SYS_*` constants and the arg structs (`spawn_args`, `ipc_call_args`, …) come
from the **same `cxk_abi.h`** the kernel and devkit already share — generated into
an X prelude (`abi.x`) so there is exactly one source of truth across kernel, C,
.NET, and X.

---

## 6. Grammar sketch (EBNF, v0.1)

```
program     = { decl } ;
decl        = fn_decl | struct_decl | global_decl | const_decl | extern_decl ;
fn_decl     = "fn" ident "(" [ params ] ")" [ "->" type ] block ;
params      = param { "," param } ;  param = ident ":" type ;
struct_decl = "struct" ident "{" [ field { "," field } ] "}" ;
field       = ident ":" type ;
type        = ("i8"|"i16"|"i32"|"u8"|"u16"|"u32"|"bool"|"void"|ident)
            | "*" type | "[" int "]" type ;
block       = "{" { stmt } "}" ;
stmt        = let | assign | if | while | return | expr ";" | block ;
let         = "let" ident [ ":" type ] "=" expr ";" ;
expr        = … (precedence climbing: || && | comparisons | + - | * / % | unary | postfix) … ;
```

---

## 7. Compiler pipeline (in `CXEX.Build`)

X plugs into the existing devkit flow; it only adds a front-end that produces
x86-32, then hands off to the emitters already in place:

```
.x source
  -> Lexer            (tokens)
  -> Parser           (X-core AST)
  -> Sema             (type-check, resolve, const-fold; rejects ambient effects)
  -> CodeGen          (X-core AST -> x86-32)
  -> [ ELF ]          (so ElfParser -> CXEXLayoutEngine -> CXEXWriter package it)
  -> .xcex / .xkex / .xoex
```

CodeGen v0.1 emits x86-32 **assembly text**, assembled+linked to ELF via the
cross toolchain the build already uses, then the existing `ElfParser` →
`CXEXWriter` path produces the CXEX — so X reuses the whole packaging chain and
only the front-end is new. (A later pass can emit machine code directly into a
CXEX section, dropping the external assembler entirely; that's a backend
implementation choice, not a language-contract change.)

New namespace, e.g. `CXEX.Build.X` (or `CXEX.Lang`): `Lexer`, `Parser`, `Ast`,
`Sema`, `CodeGen`. Output is consumed by the existing `Emitters`.

---

## 8. v0.1 milestone — the "hello in X"

Reimplement the current `hello.xcex` app in X: a `_start` that builds an
`ipc_call_args`, calls its broker endpoint (handle 0), and `exit`s. Compile it
with the .NET X front-end, package via the existing emitter, run it on CXK. When
the brokered message prints — produced by an X-compiled binary instead of C — the
**entire X toolchain is proven end to end** with a deliberately tiny language.
Then grow the surface (more operators, arrays/structs depth, modules).

Build order, mirroring the ABI: (1) freeze this core, (2) Lexer/Parser → AST,
(3) Sema, (4) CodeGen → asm/ELF, (5) `abi.x` prelude from `cxk_abi.h`,
(6) hello-in-X runs.

---

## 9. Deferred (not in core v0.1 — added on demand)

- 64-bit arithmetic, floats, SIMD.
- Modules / multi-file linking, namespaces, visibility.
- `for`, `switch`, enums, unions, function pointers (easy adds once core is solid).
- An explicit IR (only if optimization needs it).
- **XR**: runtime-as-executive-service (GC, dynamic dispatch via IPC) — a dialect
  lowering to X core + executive calls.
- **XH**: C++-like hybrid (methods, generics) — a dialect desugaring to X core.

---

*This is the v0.1 core contract. XR and XH compile by lowering to these
constructs; the backend consumes only X core. Freeze it, build the front-end,
run hello-in-X, then grow.*