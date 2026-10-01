# X — Core Language v0.3

**Status:** LIVE SPEC (core v0.3). X is the systems core of the X family. X Runtime
(runtime) and X Hybrid (hybrid) are **front-end dialects that desugar to X core** —
they are *not* separate compilers and have no independent backend. There is one
semantic core and one CXEX backend, so the dialects cannot drift.

Design rule, same as the ABI: keep the smallest useful core; add features on
demand. No separate IR yet — **X core's typed AST is the lowering target** that
X Runtime and X Hybrid reduce to and that the backend consumes. An explicit IR is added only
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

That matters more than tidiness, because **X Runtime and X Hybrid are specified by lowering to
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
| Dynamic allocation | works — `SYS_MEM_OP` and `std/heap.xfxn` |
| Growable strings & buffers | works — `std/buf.xfxn` (`buf` of bytes, `list` of u32s) |
| Tagged unions / sum types for AST nodes | works — sum types (§2.8) |
| `switch` on a tag | works — exhaustive on enums and sum types (§2.8) |
| Character literals, compound assignment, initializers | works (§2.6, §2.7, §4) |
| Recursion over a tree | works (plain recursion) |
| Multi-file source | works (`import`) |
| Function pointers for dispatch tables | works |
| File I/O | works (`SYS_FILE_OP`, `std/file.xfxn`) |
| 64/128-bit integers | works, including variable shifts, and passed and returned by value (§2.1, §2.5) |
| Structs by value | works — passed and returned by value, C-compatible (§2.5) |
| Floats | missing; not required for a compiler |

The honest read: **stages 1–3 of the route in §10 are done.** What remains is
stage 4 — writing the compiler in X — and stage 5, the assembler and linker.

---

## 1. Principles

- **Freestanding, no runtime.** X core has no GC, no allocator, no exceptions,
  no hidden calls. A compiled X program is a flat CXEX image, like the C apps today.
- **Capability-native: effects only via syscall intrinsics.** X has *no ambient
  I/O*. The only way to affect the world is `__syscall`, which lowers to `int 0x80`
  against the frozen ABI (`abi/cxk_abi.h`). So an X program is bounded by exactly
  the capability surface the kernel grants it — sandboxing falls out of the
  language, not a checker bolted on. X Runtime's "runtime" is just X code that calls
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
| Enum     | `enum` — its own integer type, or a **sum type** whose variants carry fields (§2.8) |

No floats. Pointer arithmetic is explicit and scaled by `sizeof(T)`. Integer
conversions follow §2.9: a constant goes wherever its value fits, any other
value only where no value can change, and everything else needs `as`.

### 2.1 Wide integers (64 and 128-bit)

A value wider than a register is represented by its **address**, exactly as a
`struct` or array already is. There is no second value model, which is why the
type list can grow without the backend changing: a width is a row in
`PrimWidth.Bytes`.

**Implemented:** `+ - * / % & | ^ << >>`, all six comparisons, casts between
widths (sign- or zero-filling as the source type requires), unary `-` and `~`,
and passing and returning wide values by value (§2.5).

**Shifts.** A constant amount must be inside the width — `x << 64` on a `u64`
is a compile error. A variable amount is taken **modulo the width**, which is
what x86 already does for 32-bit shifts, so the rule is the same at every width.
The variable shift is a branch-free barrel shifter: one stage per bit of the
amount (six for 64 bits, seven for 128), each a constant shift and a masked
select, so it runs the same instructions whatever the amount is and does not
leak it through timing. A narrow value may be shifted by a wide amount; only
the amount's low word counts.

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
so each of these is a compile error rather than silently wrong code:

| | why |
|---|---|
| narrowing a wide value without `as` | `let a: u32 = w;` with `w: u64` used to store `w`'s ADDRESS — in a `let`, an assignment, an argument and a return alike. Write `w as u32` to truncate on purpose |
| a constant shift outside its width | `let g: u128 = 1 << 100;` is a **32-bit** shift — both operands are small literals, and the declared type does not reach into the initializer — and the CPU would have reduced it to `1 << 4`. Give the literal the width: `1u128 << 100` (§2.6) |

**Widening is by the source's sign.** `let w: i64 = u;` with `u: u32 =
0xFFFFFFFF` is 4294967295, and an `i32` −1 widened into a `u64` is all ones —
C's rule, and what `as` always did. The implicit path used the *destination's*
sign, and gave −1 and 0x00000000FFFFFFFF.

**Mixed-width arithmetic takes the WIDER operand's type.** `0 - a` on a 64-bit
`a` is a 64-bit subtraction, not a 32-bit one. This is worth stating because it
was not always true: the result type used to be the **left** operand's, which
made `a - 0` correct and `0 - a` silently wrong — a wide value lives at an
address, so the narrow path did 32-bit arithmetic on that address and
sign-extended the result. A shift is the exception, since its right operand
counts places rather than being a term: `x << n` is as wide as `x`.

**A literal is typed by the narrowest type that holds it** — `i32` up to
2147483647, then `u32` up to 4294967295, then `u64`, then `u128`. What this buys
is that a literal stops pretending to be a type it does not fit. Every literal
used to type `i32` however large it was written, so `200000000000000000000 % 7`
truncated the constant into `eax` and did a 32-bit divide — a wrong answer from
an expression the compiler could have evaluated exactly. The `u32` band was
added later, after `0x80000000` typed as `i32` turned out to compare as
-2147483648; that is also what C does with an unsuffixed hex constant that
large.

**Ordering comparisons (`<` `<=` `>` `>=`) are unsigned if either operand is
an unsigned 32-bit value or a pointer**, and signed otherwise — C's rule. A
`u8` or `u16` widens to a signed int without losing any value, so it does not
force an unsigned comparison. This, too, was not always true: ordering used to
be signed for every operand, so a `u32` at or above 2³¹ compared as though it
were negative — `3000000000 > 5` was false. Division and `>>` already honoured
the operand type; comparison was the one left behind. The consequence of the
rule to know: `x > -1` on a `u32` is always false, because `-1` becomes
4294967295, exactly as in C.

**Why the ceiling is 128.** Up to 128 bits a value is *one thing* — an offset, a
timestamp, a GUID, a Q64.64 coordinate, the product of two 64-bit numbers — that
you compare, add, and pass by value. Past 128 it is a buffer with operations:
nobody adds two RSA moduli or orders two SHA digests by magnitude, they feed them
to an algorithm. That is a library. `kernel/lib/crypto/bignum.c` already is one,
and a future `bignum.xfxn` is the right shape for the same work in X — limbs in
an array, with the constant-time control and per-algorithm limb counts that a
fixed native width cannot express.

> **Caution for X Runtime and X Hybrid.** The comparison emitted for wide values walks limbs
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
address.

### 2.3 `defer`

`defer stmt;` or `defer { ... }` schedules a statement to run when the enclosing
**block** is left — by falling off its end, by `return`, or by `break` /
`continue` out of it. Cleanup is written next to the acquisition it undoes:

```x
let h: i32 = file_open(path, FOPEN_READ);
if (h < 0) { return h; }
defer file_close(h);            // runs on every way out of this block
let buf: u32 = alloc(4096);
if (buf == 0) { return E_NOMEM; }
defer free(buf);                // runs first: last deferred, first run
```

**The rules:**

- **Block-scoped, lexical** (as in Zig, not Go's function-scoped `defer`). A
  defer inside a loop body runs at the end of *every* iteration, a `continue`
  included.
- **Reverse order.** Innermost block first; within a block, last deferred first.
- **Only defers already reached run.** One written after a `return` in the same
  block was never registered, so it does not run — correct, because whatever it
  would release was never acquired.
- **`return x` takes `x` before the defers run**, so a defer that changes `x`
  does not change what is returned.
- **A defer body may not leave early.** `return`, `break` or `continue` out of a
  defer is an error: the body runs *during* an exit, and escaping it would
  abandon that exit and every cleanup after it. Loops wholly inside the body are
  fine.
- **Names resolve at the `defer`**, not where it runs: the body sees what was
  declared before it and nothing after.
- `defer let` is refused — it declares a variable nothing could use.

Each exit emits its own copy of the pending cleanup at the point of exit rather
than sharing one path. That needs no runtime state and no hidden record of which
exit was taken; the cost is code size, proportional to exits × deferred
statements. A function that does not use `defer` compiles to byte-identical
output.

A `defer` does not run if the process ends some other way — `exit()`, a fault,
or being killed.

### 2.4 Attributes

`@name` or `@name(args)` before a declaration. An argument is positional or
named (`@device(vendor = 0x8086)`). Several may be stacked.

**An attribute the compiler does not know is an error**, naming the ones it
does. Silently ignoring an unknown name would let a typo — `@sectoin` — compile
into a program that quietly lacks whatever the attribute was for. Each attribute
is defined in exactly one table in the type checker, together with what it
applies to and what arguments it takes; adding one means adding it there and
teaching the emitter what it does.

Named arguments are parsed today although no attribute takes one yet. The
syntax is the part that is expensive to change later, and hardware match tables
will want it.

| Attribute | Applies to | Effect |
|---|---|---|
| `@section(".name")` | a function with a body, or a global | places it in the named object-file section |

**`@section`** is the mechanism declarative tables are built on: many
declarations, in many files, landing in one section that is then read as an
array — how driver match tables will work, without a hand-maintained
registration list that drifts from the drivers.

```x
@section(".cx_tbl") global e0: u32 = 11;
@section(".cx_tbl") global e1: u32 = 0;
@section(".cx_tbl") global e2: u32 = 33;
// &e0 now points at a three-entry table, in declaration order
```

- A function's section is allocated and executable (`AX`); a global's is
  allocated and writable (`WA`).
- A zero-valued global in a named section **stays in that section** rather than
  moving to `.bss` — a section read as a table must contain every entry.
- The section name must start with `.` and contain only letters, digits, `_` and
  `.`. The default names (`.text`, `.data`, `.rodata`, `.bss`) are refused:
  naming one is redundant or, for a function in `.data`, contradictory, and the
  assembler would quietly merge section flags rather than say so.
- Refused on structs, constants, type aliases, imports, and extern functions —
  none of them occupies space to place.

A linker script that does not mention a custom section places it by the
linker's orphan rules. A table meant to be collected across files needs a
`KEEP(*(.name))` entry, and start/end symbols, in the script; that belongs with
the first real table.

Code without attributes compiles to byte-identical output. It is here now because it is cheap to add before code exists and
expensive to retrofit after, and because it is the foundation for drivers
written in X (see `CX_ROADMAP.md` §5).

### 2.5 Wide values and structs by value

A 64 or 128-bit integer or a `struct` is passed and returned **by value**, with
the i386 System V convention — what GCC does for C — so X calls C and C calls X
with ordinary prototypes:

| | how |
|---|---|
| argument | copied onto the stack, its size rounded up to 4 bytes |
| 64-bit result | in `edx:eax` |
| 128-bit or struct result | the caller passes a hidden pointer as the **first** argument; the callee writes the value through it, returns the pointer in `eax`, and pops it itself (`ret $4`) |

The callee works on its own copy: changing a parameter never changes the
caller's variable. A result is moved out before `defer`s run and before the
frame is torn down, so `return x` returns `x` as it was. Each call site that
receives one of these has its own slot in the caller's frame, sized to the
type.

Before this, all of it compiled and was wrong: a wide or struct argument pushed
its **address** and the callee read the address's bytes as the value, and a
struct result returned the address of a local in a frame that no longer existed.

An **array** cannot be passed or returned by value — C cannot either, and
copying a buffer at every call is not something to do by accident. It is a
compile error that says to pass a pointer.

### 2.6 Literals: suffixes and separators

A suffix names a literal's type outright: `1u128`, `0xFFu8`, `5i64`, any of
`u8 u16 u32 u64 u128 i8 i16 i32 i64 i128`. The value must fit — `256u8` is an
error — except that a signed literal may reach one past its maximum when it is
negated directly, so `-128i8` and the most negative `i128` can be written. An
unsuffixed literal is typed as before, by the narrowest type that holds it.

`_` separates digit groups anywhere after the first digit, as in X Data:
`1_000_000`, `0xFFFF_0000`, `0x1_0000_0000u64`.

A **character literal** is a number: `'a'` is 97. It takes the escapes a string
does, plus `\xNN` for any byte, and must be one ASCII character — anything else
is several bytes of UTF-8 and belongs in a string. It is typed `u8` but behaves
as an unsuffixed constant, so it goes into any integer type it fits (§2.9). A
lexer written in X is mostly comparisons against characters, and before this
every one was a magic number.

### 2.7 Initializers

```
let p: pt = pt { x: 1, y: 2 };                  // every field, any order
let a: [4]u32 = [10, 20, 30, 40];               // exactly as many items as the type has
global words: [2]kw = [kw { word: "if", id: 1 }, kw { word: "else", id: 2 }];
global ops: [2]fn(u32) -> u32 = [twice, thrice];
```

A struct literal names **every** field — one left out is an error, not a silent
zero, so the reader sees every value the struct starts with. An array literal is
typed by what it initializes (a `let`, an assignment, a field, an argument, a
return, a global), which fixes its element type and length. Literals nest.

In a function a literal is built in a frame slot of its own and copied like any
struct. In a **global** it is laid out as data at build time, so everything in
it must be known then: constants, string literals and function names (their
addresses) — which is exactly what keyword and dispatch tables are made of.

Arrays now copy on assignment (`b = a;` copies the bytes, exactly — a `[3]u8` is
three bytes). Before, an array assignment stored the source's **address** into
the destination.

### 2.8 Enums and sum types

```
enum color { red, green, blue }                // 0, 1, 2 - backed by u32
enum op: u8 { add = 1, sub, mul = 10, neg }    // a backing type, and values
enum node {                                    // a SUM type: variants carry fields
    num { value: i64 },
    bin { kind: op, left: *node, right: *node },
    eof,
}
```

A plain enum is **its own type**: `color.red` is a `color`, not an integer, and
crossing between them takes `as` in either direction. Variants must fit the
backing type (at most 32 bits) and must differ.

A sum type is a 4-byte tag and room for its largest variant. A value is built
with the variant's fields — `node.num { value: 5 }` — or by name when it has
none — `node.eof` — and can be copied, passed and returned like any struct. Its
fields are reached **only** through a `switch`, so they are never read under the
wrong tag:

```
switch (*n) {
    case node.num(x) { return x.value; }       // x points at the variant's fields
    case node.bin(b) { return eval(b.left) + eval(b.right); }
    case node.eof    { return 0; }
}
```

`switch` works on an integer of up to 32 bits, a plain enum or a sum type:

- labels are constants, or variants of the subject's enum; several per case
  (`case 'a', 'e' { }`); each value at most once;
- **no fallthrough**, and no `break` to leave a case — `break` and `continue`
  belong to the enclosing loop, as in Rust;
- the subject is evaluated **once**;
- with no `else`, a switch on an enum or sum type must name **every** variant,
  so adding a variant finds every switch that forgot it;
- `case T.v(name)` binds `name` to a pointer to the variant's fields, inside the
  subject itself — writes through it are seen.

`==` on a struct or array is refused: a struct is its address, so it would have
compared where two values are, not what they hold.

### 2.9 Integer conversions

The rule the spec always stated and the checker did not enforce — any integer
went into any other and was cut down by the store, so `let a: u8 = n;` with
`n = 300` kept 44:

- a **constant** (a literal, a character, a constant expression) goes wherever
  its value fits: `let c: u8 = 255;`, `let d: i8 = -128;` — unless it was
  written with a suffix, which fixes its type;
- any other value goes only where **no value can change**: an unsigned value
  into a wider type, a signed one into a wider signed type;
- everything else is an explicit `as`, which the error message names.

In arithmetic a constant takes the other operand's type when it fits it, as in
Rust: `x + 1` on a `u8` is a `u8`. Compound assignment narrows back, as in C:
`b += 1` on a `u8` is fine. Any two integers may be compared.

Auditing all of CXK's X code against this found one site: `return 0 - 1;` from
a `u32` function — a sentinel, now written `0xFFFFFFFF`.

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
- Functions use the i386 System V (cdecl) calling convention, including how
  wide values and structs are passed and returned (§2.5), so X, assembly and C
  call one another directly. `cxk compile --object` makes X linkable into a C
  program; the kernel links its X Data reader that way.

---

## 4. Statements & expressions

```
let x: i32 = expr;        // local; type optional if inferable from initializer
let buf: [64]u8;          // declared, UNINITIALIZED (type required; not zeroed)
x = expr;                 // assignment
x += expr;                // and -= *= /= %= &= |= ^= <<= >>=
if (cond) { } else { }
while (cond) { }
switch (x) { case 1, 2 { } else { } }   // §2.8
break;  continue;         // innermost loop only; checked at type-check time
return expr;              // or bare `return;`
expr;                     // expression statement (e.g. a call)
```

`let` without an initializer is what makes local buffers and structs practical
(`let a: fb_op_args;`). It follows C: the slot is reserved and **not zeroed**, so
every field you care about must be assigned before use. The type is required —
there is nothing to infer from.

`x op= v` is `x = x op v` with `x` written once. The target is evaluated twice,
so one containing a call is refused — the call would run twice. A local declared
without a type (`let x = 5;`) takes its initializer's; until now such a local
read as `void` wherever it was used.

Expressions: integer/bool/**character**/**string** literals, struct and array
literals (§2.7), identifiers, calls `f(a, b)`,
member `s.field`, deref `*p`, address-of `&x`, index `a[i]`, `sizeof expr`, the
arithmetic/comparison/logical operators, the **bitwise** operators `& | ^ ~` and
**shifts** `<< >>`, and `expr as T` casts.

- Precedence follows C, including C's famous choice that `&` binds looser than
  `==`. Parenthesise bit tests: `(flags & MASK) != 0`.
- `>>` is arithmetic on signed operands and logical on unsigned; `/` and `%` use
  signed or unsigned division according to the left operand's type. Type aliases
  are expanded before that decision.
- Overflow is **defined** — wraps, two's complement, **at the type's width**.
  No UB; X is meant to be predictable. An 8 or 16-bit result is wrapped as it is
  computed: `b + 1` on a `u8` of 255 is 0, and `(x as u8) == 44` for x = 300.
  Before, both kept the full 32-bit register value until a store happened to
  truncate it, so they were right in a variable and wrong in an expression.
- A string literal has type `*u8` and points at pooled, NUL-terminated bytes, so
  it works both as a counted buffer and as a C-style string. Identical literals
  share storage. Escapes: `\n \t \r \0 \b \f \v \a \e \\ \" \'`.
- `as` is the one way to change an integer's type where the value might not
  survive; it truncates or extends, as C does.

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
- `for` (a `while` does the job), untagged unions (a sum type is the safe one),
  `is` to test a variant without a switch.
- Floats and SIMD. (Fixed-point lives in `std/fixed.xfxn`; the kernel
  initialises the FPU but no userspace code uses it.)
- An explicit IR (only if optimization needs it).
- **X Runtime**: runtime-as-executive-service (GC, dynamic dispatch via IPC) — a dialect
  lowering to X core + executive calls.
- **X Hybrid**: C++-like hybrid (methods, generics) — a dialect desugaring to X core.

### Known rough edges (not features — defects to fix)
- `as` between integers of different widths **truncates or extends** as C does;
  nothing reports a value that does not survive. That is what `as` is for, but a
  checked conversion (`try_as`, returning whether it fit) would be worth having.
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

*Stages 1–3 are done* — file I/O and `exec_path`; `SYS_MEM_OP` and
`std/heap.xfxn`; `std/buf.xfxn`, sum types and `switch`. Stage 4 is next.

**Stage 3 — the data types a compiler is made of.** Growable byte buffers and
strings in `std/`, then sum types with `switch` in the language. An AST is a tagged
union and a token stream is a growable buffer; emulating either with structs and
manual tags is the point where hand-written X stops scaling.

**Stage 4 — bootstrap.** Write the X compiler in X and compile it with the C#
front-end. The result runs on CXK. From then on the C# implementation is a
bootstrap artifact, kept only to rebuild from scratch.

*Done, in `os/xc`.* Built one stage at a time, each held to the C# compiler by
a differential test before the next began:

| | X | held to C# by | status |
|---|---|---|---|
| lexer | `xc/lex.xfxn` | `tests/xc/lexdiff.py` in CX_DEVKIT: every X source, plus thousands of mutated ones, token for token | **done** |
| parser | `xc/parse.xfxn` | `tests/xc/parsediff.py`: every X source, plus thousands of broken ones, node for node and error for error | **done** |
| type checker | `xc/sema.xfxn`, `xc/front.xfxn` | `tests/xc/semadiff.py`: whole programs - prelude, imports and all - every diagnostic in order and every expression's type | **done** |
| code generator | `xc/emit.xfxn`, `xc/xc.xfxn` | `tests/xc/asmdiff.py`: whole programs, the assembly character for character - and `tests/xc/selfhost.py` | **done** |

**The compiler compiles itself.** `xc` takes an X program to the assembly
`cxk compile` writes for it, exactly. `tests/xc/selfhost.py` is the classic
bootstrap check: the C# compiler builds xc; that xc builds xc; that xc builds
xc again - and all three assemblies are identical, 67,000 lines of them. The
compiler that X built then compiles every program in `tests/lang/run`, and
each runs correctly. On CXK, in `/Shared/Source` - where the build stages xc's
whole source, the CXK platform file as `xc_sys.xfxn` -

    run +disk xc --prelude prelude.xfxn xc.xfxn

writes `xc.s`, 1,258,932 bytes with the same hash as on the build machine: the
X compiler, compiled on the system it is written for, by itself. Assembling and
linking are still the cross toolchain's; that is stage 5.

Fitting in a CXK process took work. Its nodes now live in chunks that never
move - one doubling array left every smaller copy behind in a heap that does
not give memory back - and xc writes its assembly as it goes rather than
holding it, which together took a self-compile from 15 MB of heap to under 8,
inside the 16 MB a process gets.

**Two platforms, one compiler.** The same sources run on CXK and on the Linux
host it is developed on. A platform file supplies what differs - the entry
(`xc_main`), arguments, reading a file, writing output - and is chosen by the
build's `-I`: `xc/cxk/xc_sys.xfxn` uses the prelude and `std/file.xfxn`;
`xc/host/xc_sys.xfxn` uses Linux's i386 system calls through the same
`int 0x80`, and backs `mem_op` with `brk` so the real `std/heap.xfxn` and
`std/buf.xfxn` run unchanged. That is what makes the differential tests cheap:
thousands of files per minute, natively, with no machine to boot.

**The lexer on CXK.** `tokdump` (built into the image, `/Shared/Programs`)
prints tokens in the format of `cxk tokens`; `-s` prints a count and a hash
instead. `run +disk tokdump -s /Shared/Source/lex.xfxn` on CXK gives the same
4207 tokens and hash as the host - the lexer, written in X, reading its own
source on the system it is for. (`+disk` because `run` grants only the console
unless asked.)

**The parser.** A line-for-line port of the C# one: the same grammar and
precedence, and - what the mutants test hardest - the same recovery after a
mistake and the same messages, at the same line and column. Its tree is one
array of uniform nodes named by index (`node` in `parse.xfxn` documents what
each kind's fields hold), so it grows with one reallocation and no pointer
into it is kept across parsing. `astdump` prints it in the format of
`cxk ast`; on CXK, `run +disk astdump -s /Shared/Source/parse.xfxn` gives the
same 5404 nodes and hash as the host - the parser reading its own source.

**The type checker.** Resolution, constant folding and checking, ported from
the C# `Resolver`, `ConstFold` and `TypeChecker`, behind a front end that
builds a program the way `cxk compile` does: the prelude (from a file - `cxk
prelude` writes it) in front of the main file, every import read once,
breadth first, and one diagnostic stream in the C# order. The lexer gained
the C# lexer's error messages for it. `semadump` prints what it concludes in
the format of `cxk sema`; on CXK, `run +disk semadump -s --prelude prelude.xfxn
sema.xfxn` in `/Shared/Source` type checks the type checker - 471
declarations, no errors, the same hash as the host.

Porting it meant reading the C# compiler more closely than anything had, and
that found real bugs, all fixed in both. The code generator added two more: a
type too large to count - `let a: [0x7FFFFFFF]u32;` - wrapped the frame size
negative, so the function reserved no stack at all; and a struct holding itself
by value crashed the compiler sizing it. Both are now type errors (nothing over
1 GB; nothing with no size). Before those, the type checker's: `~` was not folded (`const M: u32 =
~0;` was 0); a constant's value was not cut to its type when loaded; a 64 or
128-bit constant crashed any program that read it; a constant defined through
itself and a sum type containing itself crashed the compiler; locals were kept
by NAME, so an inner `let x` replaced an outer one for the whole function; a
nested wide expression lost its left half (`a | b | c`); and every function in
a program got the wide-temporary pool if any did, which is what pushed the type
checker past CXK's 16 KB user stack (now 256 KB). The disk layer also learned
to split transfers, since AHCI moves at most 128 sectors at a time and the
installer read each staged file whole.

Comparing the two lexers fixed the C# one twice: a string ending in a
backslash at the end of a file crashed it, and identifiers accepted any Unicode
letter, which made the language depend on the host's Unicode tables. Both are
ASCII-only now, and both count a column per character.

**Stage 5 — the rest of the toolchain.** Assembler and linker, or a backend that
emits CXEX sections directly and drops the external assembler (v0.1 §7 already
names this as a backend choice, not a language change). Until this lands,
self-hosting is partial: X could compile X but would still need the cross
toolchain to package it.

A text editor is a prerequisite for any of this being pleasant, and is not on this
list because it needs nothing beyond Stage 1.

---

*This is the v0.2 core spec. X Runtime and X Hybrid compile by lowering to these constructs;
the backend consumes only X core. The core is stable, not frozen: when a feature
lands, it is documented here in the same change.*