# X — Core Language v0.2

**Status:** LIVE SPEC (core v0.2). X is the systems core of the X family. XR
(runtime) and XH (hybrid) are **front-end dialects that desugar to X core** —
they are *not* separate compilers and have no independent backend. There is one
semantic core and one CXEX backend, so the dialects cannot drift.

Design rule, same as the ABI: keep the smallest useful core; add features on
demand. No separate IR yet — **X core's typed AST is the lowering target** that
XR/XH reduce to and that the backend consumes. An explicit IR is added only
if/when optimization needs it.

---

## 0. Why this is v0.2

v0.1 was written as a FROZEN CONTRACT, and then the implementation outgrew it —
correctly, because the language was being used to write a real shell and a real
GUI. Features listed in v0.1 §9 as *deferred* shipped anyway: function pointers,
modules, `break`/`continue`, `sizeof`, string literals, the bitwise and shift
operators, and type aliases. The frozen grammar described a language that no
longer existed.

That matters more than tidiness, because **XR and XH are specified by lowering to
this document.** A dialect written against a stale core is specified against a
fiction. So v0.2 records the language as built, and drops the word "frozen" for
an honest rule: *the core is stable, not frozen — additions are documented here
when they land, and the `abi.x` prelude plus the CXEX backend are the contract
that must not break.*

### The north star: X hosted on CXK

**The long-term goal is to build CXK from CXK** — the system compiling its own
software, on itself, without a Windows or .NET host. Today the X front-end is C#
(`CXEX.Lang`, running on .NET), so CXK can run X programs but cannot produce
them. That single fact is what currently prevents the OS from being
self-sufficient, and it shapes what X needs next.

Self-hosting is a demanding target and it should drive feature priority, because
a compiler is a specific kind of program:

| A self-hosting compiler needs | v0.2 status |
|-------------------------------|-------------|
| Dynamic allocation | **missing** — no allocator; needs `sbrk`/`map` (ABI §7.8) or an arena in X |
| Growable strings & buffers | **missing** — fixed arrays only |
| Tagged unions / sum types for AST nodes | **missing** — would be structs + a manual tag today |
| `switch` on a tag | **missing** — `if`/`else` chains work but scale badly |
| Recursion over a tree | works (plain recursion) |
| Multi-file source | works (`import`) |
| Function pointers for dispatch tables | works |
| File I/O | **missing** — no FS syscalls yet (ABI §7.10) |
| 64-bit integers | missing; probably not required |
| Floats | missing; not required for a compiler |

The honest read: **the blockers are allocation, strings, and sum types** — not
syntax sugar. A staged route is in §10.

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

// module import — pulls another source file into the same compilation unit
import "gfx.xfxn";

// type alias — TRANSPARENT: `fx` and `i32` are the same type, not distinct
type fx = i32;
type layer_draw_fn = fn(u32, u32, u32, u32, u32) -> void;   // function pointer type
```

- Entry point is `fn _start() -> void` (matches the CXEX entry convention).
- **Source files use the `.xfxn` extension**; the generated ABI prelude is `abi.x`.
- `import` is *textual module merging*, not separate compilation: every imported
  file's declarations join one flat namespace, resolved as a whole program. Imports
  are deduplicated by resolved path, so a diamond or a cycle is harmless. Search
  order is the importing file's directory, then the main source's directory, then
  any `-I` directory, then `std/` beside the compiler. Consequences worth knowing:
  there is no visibility control and no per-module namespacing, so two libraries
  defining the same name collide; and the `abi.x` prelude is prepended only to the
  main file, so an imported library must not import it again.
- **Type aliases are transparent.** `type fx = i32;` makes `fx` interchangeable
  with `i32` everywhere. Making them distinct would catch `fx_mul(count, 5)` but
  needs conversion rules, so transparency is the current choice. Every place the
  backend decides something from a type — size, signedness, struct-ness, array
  decay — must expand aliases first.
- **Function pointers** are first-class values: a bare function name evaluates to
  its address, a `fn(...) -> R` type holds one, and calling through a variable,
  parameter or struct field emits an indirect call. This is what the GUI's layer
  compositor dispatches through.
- Functions use cdecl-compatible calling convention (args on stack or the agreed
  register set), so X, asm, and the C bootstrap interoperate during the transition.

---

## 4. Statements & expressions

```
let x: i32 = expr;        // local; type optional if inferable from initializer
let buf: [64]u8;          // declared, UNINITIALIZED (type required; not zeroed)
x = expr;                 // assignment
if (cond) { } else { }
while (cond) { }
break;  continue;         // innermost loop only; checked at type-check time
return expr;              // or bare `return;`
expr;                     // expression statement (e.g. a call)
```

`let` without an initializer is what makes local buffers and structs practical
(`let a: fb_op_args;`). It follows C: the slot is reserved and **not zeroed**, so
every field you care about must be assigned before use. The type is required —
there is nothing to infer from.

Expressions: integer/bool/**string** literals, identifiers, calls `f(a, b)`,
member `s.field`, deref `*p`, address-of `&x`, index `a[i]`, `sizeof expr`, the
arithmetic/comparison/logical operators, the **bitwise** operators `& | ^ ~` and
**shifts** `<< >>`, and `expr as T` casts.

- Precedence follows C, including C's famous choice that `&` binds looser than
  `==`. Parenthesise bit tests: `(flags & MASK) != 0`.
- `>>` is arithmetic on signed operands and logical on unsigned; `/` and `%` use
  signed or unsigned division according to the left operand's type. Type aliases
  are expanded before that decision.
- Overflow is **defined** — wraps, two's complement. No UB; X is meant to be
  predictable.
- A string literal has type `*u8` and points at pooled, NUL-terminated bytes, so
  it works both as a counted buffer and as a C-style string. Identical literals
  share storage. Escapes: `\n \t \r \0 \b \f \v \a \e \\ \" \'`.
- `as` currently **reinterprets** and does not convert: narrowing casts do not
  mask. This is a known rough edge, not a design position.

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
decl        = fn_decl | struct_decl | global_decl | const_decl | extern_decl
            | import_decl | type_decl ;
fn_decl     = "fn" ident "(" [ params ] ")" [ "->" type ] block ;
params      = param { "," param } ;  param = ident ":" type ;
struct_decl = "struct" ident "{" [ field { "," field } ] "}" ;
field       = ident ":" type ;
import_decl = "import" string ";" ;
type_decl   = "type" ident "=" type ";" ;
type        = ("i8"|"i16"|"i32"|"u8"|"u16"|"u32"|"bool"|"void"|ident)
            | "*" type | "[" int "]" type
            | "fn" "(" [ type { "," type } ] ")" [ "->" type ] ;
block       = "{" { stmt } "}" ;
stmt        = let | assign | if | while | return | "break" ";" | "continue" ";"
            | expr ";" | block ;
let         = "let" ident [ ":" type ] [ "=" expr ] ";" ;   /* type required if no init */
expr        = … precedence, loosest to tightest:
              ||  &&  |  ^  &  ==/!=  </<=/>/>=  <</>>  +/-  */ /%  unary  postfix … ;
unary       = "-" | "!" | "~" | "*" | "&" | "sizeof" ;
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
  -> .xuex / .xkex / .xoex
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

## 8. v0.1 milestone — the "hello in X" (achieved, and long since passed)

> **Status: done and superseded.** The toolchain is proven end to end, and X has
> gone well past hello: the entire ring-3 userland is now written in it — a shell
> with a command table, a `std/` of eight libraries (`io`, `string`, `mem`, `fmt`,
> `fixed`, `gfx`, `gui`, `net`), and a windowed GUI with a layer compositor. The
> original milestone is kept below as the record of what "proven end to end" meant.


Reimplement the current `hello.xuex` app in X: a `_start` that builds an
`ipc_call_args`, calls its broker endpoint (handle 0), and `exit`s. Compile it
with the .NET X front-end, package via the existing emitter, run it on CXK. When
the brokered message prints — produced by an X-compiled binary instead of C — the
**entire X toolchain is proven end to end** with a deliberately tiny language.
Then grow the surface (more operators, arrays/structs depth, modules).

Build order, mirroring the ABI: (1) freeze this core, (2) Lexer/Parser → AST,
(3) Sema, (4) CodeGen → asm/ELF, (5) `abi.x` prelude from `cxk_abi.h`,
(6) hello-in-X runs.

---

## 9. Delivered since v0.1, and what is still deferred

### Delivered (was "deferred" in v0.1 §9)
Modules / multi-file source (`import`), function pointers, `break`/`continue`,
`sizeof`, string literals, bitwise operators and shifts, type aliases,
uninitialized `let`.

### Still deferred
- **Namespaces / visibility.** `import` merges into one flat namespace; there is
  no `pub`, no module-qualified names, and name collisions across libraries are a
  hard error.
- `for`, `switch`, enums, unions.
- 64-bit arithmetic, floats, SIMD. (Fixed-point lives in `std/fixed.xfxn`; the
  kernel initialises the FPU but no userspace code uses it.)
- Dynamic allocation of any kind.
- An explicit IR (only if optimization needs it).
- **XR**: runtime-as-executive-service (GC, dynamic dispatch via IPC) — a dialect
  lowering to X core + executive calls.
- **XH**: C++-like hybrid (methods, generics) — a dialect desugaring to X core.

### Known rough edges (not features — defects to fix)
- `as` reinterprets rather than converts; narrowing does not mask.
- `sizeof` accepts only expressions, not type names, and silently yields `4` when
  an operand's type is unknown rather than erroring.
- A cyclic `type` alias hits a depth guard and returns a half-expanded type
  instead of reporting the cycle.
- `/` and `%` consult only the **left** operand's signedness; mixed-sign division
  follows it silently.

---

## 10. Route to self-hosting

The order below is chosen so each step is independently useful — nothing is built
purely in service of the compiler.

**Stage 1 — make the OS able to hold source at all.** Filesystem access from ring
3 (`docs/CX_ABI.md` §7.10) and `exec_path`. Without file I/O there is no reading a
`.xfxn` and no writing a `.xuex`, so this gates everything. Useful on its own: it
is what lets the shell and GUI launch application files.

**Stage 2 — memory.** `sbrk` or `map` (ABI §7.8) plus an allocator written in X.
A compiler cannot work in fixed arrays. Useful on its own: every non-trivial app
needs it.

**Stage 3 — the data types a compiler is made of.** Growable byte buffers and
strings in `std/`, then sum types with `switch` in the language. An AST is a tagged
union and a token stream is a growable buffer; emulating either with structs and
manual tags is the point where hand-written X stops scaling.

**Stage 4 — bootstrap.** Write the X compiler in X and compile it with the C#
front-end. The result runs on CXK. From then on the C# implementation is a
bootstrap artifact, kept only to rebuild from scratch.

**Stage 5 — the rest of the toolchain.** Assembler and linker, or a backend that
emits CXEX sections directly and drops the external assembler (v0.1 §7 already
names this as a backend choice, not a language change). Until this lands,
self-hosting is partial: X could compile X but would still need the cross
toolchain to package it.

A text editor is a prerequisite for any of this being pleasant, and is not on this
list because it needs nothing beyond Stage 1.

---

*This is the v0.2 core spec. XR and XH compile by lowering to these constructs;
the backend consumes only X core. The core is stable, not frozen: when a feature
lands, it is documented here in the same change.*