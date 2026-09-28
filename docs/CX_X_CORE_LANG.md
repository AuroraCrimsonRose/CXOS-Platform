# X — Core Language v0.3

**Status:** LIVE SPEC (core v0.3). X is the systems core of the X family. XR
(runtime) and XH (hybrid) are **front-end dialects that desugar to X core** —
they are *not* separate compilers and have no independent backend. There is one
semantic core and one CXEX backend, so the dialects cannot drift.

Design rule, same as the ABI: keep the smallest useful core; add features on
demand. No separate IR yet — **X core's typed AST is the lowering target** that
XR/XH reduce to and that the backend consumes. An explicit IR is added only
if/when optimization needs it.

---

## 0. Why this is v0.3

v0.2 recorded the language as built and dropped the word "frozen" for a rule:
*additions are documented here when they land.* v0.3 is that rule being kept.
64 and 128-bit integers shipped, file I/O shipped, and the type table still
described neither — which is precisely the failure v0.2 was written to end,
recurring in a single afternoon rather than over a version. §2.1 is new, and the
self-hosting table in §1 now reflects what actually works.

## 0.1 Why there was a v0.2

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
| File I/O | works (`SYS_FILE_OP`, `std/file.xfxn`) |
| 64/128-bit integers | works, except variable shifts (§2.1) |
| Floats | missing; not required for a compiler |
| Dynamic allocation | **missing** — no `SYS_MAP`/`SBRK`, no allocator |

The honest read: **the blockers are allocation, strings, and sum types** — not
syntax sugar. A staged route is in §10.

Allocation is now the single largest one, and it blocks more than self-hosting:
XR's collector and XH's managed references both need a heap, and there is none.
`GRANT_MEM` exists and nothing honours it — ABI §7.8 is still *specified,
unimplemented*.

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

## 2. Types

| Category | Types |
|----------|-------|
| Signed   | `i8 i16 i32 i64 i128` |
| Unsigned | `u8 u16 u32 u64 u128` |
| Boolean  | `bool` (1 byte; `true`/`false`) |
| Pointer  | `*T` (32-bit) |
| Void     | `void` (return type only) |
| Aggregate| `struct` (named, by-value or via pointer) |
| Array    | `[N]T` (fixed size) |

No floats. Pointer arithmetic is explicit and scaled by `sizeof(T)`. No implicit
conversions except literal→sized-int where it fits; everything else needs a cast
`as`.

### 2.1 Wide integers (64 and 128-bit)

A value wider than a register is represented by its **address**, exactly as a
`struct` or array already is. There is no second value model, which is why the
type list can grow without the backend changing: a width is a row in
`PrimWidth.Bytes`.

**Implemented:** `+ - * / % & | ^`, all six comparisons, casts between widths
(sign- or zero-filling as the source type requires), and unary `-` and `~`.
Shifts take a **constant** amount.

`/` and `%` are one routine: a division produces the quotient and the remainder
together, and computing `a / b` and `a % b` separately would run the whole thing
twice. It is **restoring shift-subtract long division** — one iteration per bit,
so 64 or 128 of them. That is slow, deliberately: Knuth's algorithm D is several
times faster and is the right answer eventually, but it is fiddly in exactly the
places that produce answers which are *almost* right, and wide types exist here
to stop silent numeric errors. Signs are handled outside the loop, which only
sees magnitudes; the remainder takes the sign of the **dividend**, so `-7 % 2` is
`-1`.

Division by zero raises `#DE`, by deliberately executing a 32-bit divide by zero.
That makes wide division behave exactly as 32-bit division already does, and
CXK's ring-3 fault handler ends the offending process rather than the machine. A
software check returning 0 would have invented a second, quieter rule for the
same mistake.

> **Not constant time.** The trip count is fixed at the operand width, so it does
> not leak the size of either operand — but the two arms of the trial subtraction
> differ in length. Do not divide secret values. Same caveat as the wide compare.

**Diagnosed, not emitted** — a wrong answer here is worse than a build failure,
so each of these is a compile error rather than silently truncated code:

| | why |
|---|---|
| variable shifts | needs a loop, and a loop whose trip count depends on the operand is a timing signal |
| returning a wide value | the value is an address, and it would be the address of a frame about to be torn down. Pass a pointer to the destination instead |
| a constant expression at a width its operands do not have | `let g: u128 = 1 << 100;` is a **32-bit** shift, because both operands are small literals and the declared type does not reach into the initializer. C has the same rule and answers it with a suffix (`1ULL`); X has no suffix, so the width has to come from a variable that already carries it: `let g: u128 = 1; g = g << 100;` |

**Mixed-width arithmetic takes the WIDER operand's type.** `0 - a` on a 64-bit
`a` is a 64-bit subtraction, not a 32-bit one. This is worth stating because it
was not always true: the result type used to be the **left** operand's, which
made `a - 0` correct and `0 - a` silently wrong — a wide value lives at an
address, so the narrow path did 32-bit arithmetic on that address and
sign-extended the result. A shift is the exception, since its right operand
counts places rather than being a term: `x << n` is as wide as `x`.

**A literal is typed by the narrowest type that holds it** — `i32` up to 32
bits, then `u64`, then `u128`. Anything that fitted before still types `i32`,
so nothing changed shape; what this buys is that a literal too wide for a
register stops pretending to be one. Every literal used to type `i32` however
large it was written, so `200000000000000000000 % 7` truncated the constant
into `eax` and did a 32-bit divide — a wrong answer from an expression the
compiler could have evaluated exactly.

**Why the ceiling is 128.** Up to 128 bits a value is *one thing* — an offset, a
timestamp, a GUID, a Q64.64 coordinate, the product of two 64-bit numbers — that
you compare, add, and pass by value. Past 128 it is a buffer with operations:
nobody adds two RSA moduli or orders two SHA digests by magnitude, they feed them
to an algorithm. That is a library. `kernel/lib/crypto/bignum.c` already is one,
and a future `bignum.xfxn` is the right shape for the same work in X — limbs in
an array, with the constant-time control and per-algorithm limb counts that a
fixed native width cannot express.

> **Caution for XR and XH.** The comparison emitted for wide values walks limbs
> from the top down and **branches on the first difference**. That is a timing
> oracle on secret data. Any dialect exposing these types to cryptographic code
> needs a constant-time comparison primitive, not this one.

### 2.2 Address spaces

A pointer is a number, and every pointer is the same width. Without a way to
say *which memory* the number names, a physical address, a device address, an
address a ring-3 caller passed in, and an ordinary pointer are all one type —
and the two classic kernel catastrophes, **reading through an unvalidated user
pointer** and **handing a device a virtual address**, become things the compiler
cannot see.

| Type | Names | Dereference |
|---|---|---|
| `*T` | memory in the address space this code runs in | yes |
| `*user T` | an address a less-trusted caller chose | **no** — validate, then convert |
| `*phys T` | a physical address | **no** — translate to virtual first |
| `*dma T` | the address a device sees | **no** — never the CPU's to use |

Plain `*T` is the kernel's own pointer in kernel code and the program's own in
userland; there is no `*kernel`, because it would mean the same thing.

**The rules:**

- Different address spaces are **different types**. Assigning, passing,
  returning or comparing across them is an error.
- `*p`, `p[i]` and `p.field` are all refused on a qualified pointer — one check
  shared by all three, so none of them is the way around the others.
- A pointer **cannot be cast straight to a pointer in another space.**
  `p as *u8` on a `*phys` is not a conversion; the number is unchanged and it
  names different memory. Every real crossing is *arithmetic* — phys to virt
  adds an offset, validating a user pointer checks a range — so the route is
  through an integer, which is where that arithmetic goes:

  ```x
  fn phys_to_virt(p: *phys u8) -> *u8 { return ((p as u32) + KERNEL_VBASE) as *u8; }
  ```

  An untranslated crossing (`p as u32 as *u8`) is still possible, but it is
  visibly deliberate rather than hidden in a cast.
- Pointer↔integer casts stay free in every space: that is how an address arrives
  from a device register or a syscall argument in the first place.

`user`, `phys` and `dma` are **contextual**, not reserved: they are qualifiers
only directly after `*` and before another type. A variable or struct named
`user` still works, and `*user` alone is a pointer to that struct.

**No code generation changes.** This is a type-checker property only; the same
program with and without qualifiers emits byte-identical assembly.

It has no users in the OS yet — nothing in userland holds a physical or device
address. It is here now because it is cheap to add before code exists and
expensive to retrofit after, and because it is the foundation for drivers
written in X (see `CX_ROADMAP.md` §5).

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
- Floats and SIMD. (Fixed-point lives in `std/fixed.xfxn`; the kernel
  initialises the FPU but no userspace code uses it. 64 and 128-bit integers
  landed — see §2.1 — but `/`, `%` and variable shifts are still diagnosed
  rather than emitted.)
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