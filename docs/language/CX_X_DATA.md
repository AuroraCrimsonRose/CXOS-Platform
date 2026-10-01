# X Data
### CX Design Spec — Aurora Tejeda / CATX SYSTEMS LLC

> **Status: v0, reader implemented** (`os/std/xdata.xfxn`), with **service
> descriptors (`.xosv`)** and **the kernel's own configuration
> (`/System/Config/kernel.xkco`)** as consumers, both checked at build time by
> the DevKit's port of the same reader (§6a). The kernel links the X reader
> itself (§6b). Writing and editing, and schemas, are
> specified as intent in §7 and are not built yet.

X Data is CXOS's format for configuration and structured data: service
descriptors, package manifests, system / user / workspace settings, and anything
else that a person writes by hand and a program reads. Its source extension is
**`.xfxd`** — domain **F**, object type **`xd`** — and other extensions may carry
X Data content where the file's *purpose* deserves its own name, as `.xosv`
service descriptors do.

It is written in full as **X Data**, never abbreviated; see `CX_ROADMAP.md` §2.

---

## 1. Why not JSON, TOML or RON

| | Problem it has | X Data |
|---|---|---|
| JSON | no comments; every key quoted; trailing commas are errors; no symbols | comments, bare keys, trailing commas, symbols |
| TOML | tables-of-tables syntax gets awkward for nesting; dates and floats CXOS does not need yet | nested records are just `{ }` |
| RON | `:` for fields reads like a type annotation; parentheses for structs | `=` for fields, `{ }` for records |
| the old `.xosv` | untyped text; `args=` split on spaces so an argument containing a space could not be written | typed values; lists are lists |

---

## 2. A document

```x
// A document is a record whose braces are implied.
name    = "hello"
exec    = "/Shared/Programs/hi.xuex"
args    = ["started by", "the supervisor"]
start   = boot                  // a symbol: a name, not a string
every   = 300                   // an integer
enabled = true
limits  = {                     // a nested record
    memory = 0x0100_0000
    files  = 16,                // commas are optional at a line end
}
owner   = Service { id = 7 }    // a record with a type tag
```

---

## 3. Values

| Kind | Written | Notes |
|---|---|---|
| **string** | `"text"` | escapes `\n \t \r \0 \\ \"`; a raw newline inside is an error |
| **integer** | `300`, `-12`, `0x1F`, `1_000_000` | `_` may separate digits; the range is checked when it is *read*, against the type asked for |
| **bool** | `true`, `false` | |
| **symbol** | `boot`, `console` | a bare name: a value drawn from a known set, not free text |
| **list** | `[a, b, c]` | any values, not necessarily of one kind |
| **record** | `{ key = value }` | keys unique within one record |
| **tagged record** | `Name { ... }` | the tag names what the record *is*; `{` must be on the same line as the tag |

There are **no floating-point numbers** — X has none, and nothing that reads X
Data needs them yet — and **no null**: a key that has no value is absent.

**Symbols exist because most configuration values are not text.** `start = boot`
names one of a known set of moments; `"boot"` would be a string that merely
happens to be spelled like one. A reader that asks for a symbol and finds a
string is told so, which is how a typo like `start = "boto"` gets caught.

### Names

Keys, symbols and tags are **identifiers**: a letter or `_`, then letters,
digits, `_` or `-`. `true` and `false` are bools, not symbols.

Quoted keys are not in v0. Every key needed so far is an identifier, and adding
quoted keys later breaks nothing.

---

## 4. Layout

- **Comments**: `// to the end of the line` and `/* block */`. Block comments do
  not nest. X Data uses X's comment syntax, not `#`, so one family has one
  comment style.
- **Separators**: between two entries, or two list items, there must be a comma
  **or** a line break. `a = 1 b = 2` on one line is an error, not two entries —
  every value is self-delimiting, so it *could* be accepted, but a missing comma
  on one line is far more often a mistake than a style.
- **Trailing commas** are always allowed.
- **Whitespace** is spaces, tabs, carriage returns and line breaks.
- The text is UTF-8. Identifiers are ASCII; strings may hold any UTF-8.

---

## 5. Strictness

A document is **checked whole before anything reads it.** Either all of it is
well-formed or none of it is used — a reader never acts on the first half of a
file and then discovers the second half is broken.

Refused, each with its own error and the byte offset where it happened:

| Error | Meaning |
|---|---|
| `XD_E_SYNTAX` | a character that cannot start or continue what is expected here |
| `XD_E_STRING` | an unterminated string, a raw newline inside one, or an unknown escape |
| `XD_E_DEPTH` | nested more than 32 levels deep |
| `XD_E_DUPKEY` | a key repeated within one record — the second would silently win in most formats; here it is an error |
| `XD_E_SEP` | two entries or items on one line with no comma between |

And, when a value is read:

| Error | Meaning |
|---|---|
| `XD_E_TYPE` | the value is not the kind asked for — a string where a symbol was expected |
| `XD_E_RANGE` | an integer does not fit the type asked for |
| `XD_E_SPACE` | the caller's buffer is too small for the string |

The depth limit exists so a hostile file cannot exhaust the reader's stack.

---

## 6. The reader (`os/std/xdata.xfxn`)

**Allocates nothing and depends on nothing** — not the allocator, not the
prelude, not a syscall. That keeps it usable anywhere, including before the
heap exists and eventually in the kernel, and it is why its tests run natively.

A value is described by an `xd_val`: its kind and where it sits in the source.
Nothing is copied until the caller asks for it.

```x
let v: xd_val;
if (xd_check(src, len) != XD_OK) { /* xd_err_at says where */ }
xd_root(src, len, &root);
if (xd_get(src, &root, "every", &v) == 1) { xd_u32(src, &v, &every); }
```

| Function | Does |
|---|---|
| `xd_check(src, len)` | validates the whole document; `XD_OK` or an error, with `xd_err_at` set |
| `xd_root(src, len, out)` | the document's implied top-level record |
| `xd_get(src, rec, key, out)` | 1 and the value if `key` is in the record, else 0 |
| `xd_item(src, list, i, out)` | 1 and the `i`th item, else 0 past the end |
| `xd_count(src, v)` | items in a list, or entries in a record |
| `xd_str(src, v, buf, cap)` | the string, unescaped and NUL-terminated; its length or an error |
| `xd_u32` / `xd_i32(src, v, out)` | the integer, range-checked for that type |
| `xd_bool(src, v, out)` | the bool |
| `xd_sym_is(src, v, name)` | 1 if `v` is the symbol `name` |
| `xd_tag_is(src, rec, name)` | 1 if the record carries the tag `name` |
| `xd_entry_at(src, rec, i, key, val)` | 1 and the `i`th entry's key and value, else 0 — for a reader that must see every key |
| `xd_strerror(code)` | a short description of an `XD_E_*` code |
| `xd_line(src, at)` | the 1-based line a byte offset falls on — `xd_err_at` made findable |

**A reader should refuse keys it does not know.** `xd_get` only finds what it
is asked for, so a misspelt key is otherwise silently ignored. Walking the keys
with `xd_entry_at` and refusing any that are not expected is what turns
`evry = 60` from a service that quietly never repeats into an error on line 3.
Until schemas exist (§7), that check belongs to each reader.

### First consumer: service descriptors

`/System/Services/*.xosv` are X Data. The supervisor reads each one whole,
refuses unknown keys and wrong-kind values with the line they are on, and starts
nothing from a descriptor it could not read completely:

```x
exec   = "/Shared/Programs/hi.xuex"
args   = ["started by", "the supervisor"]   // argv[1] is "started by", one argument
start  = boot
every  = 300
grants = [console, disk]
```

`xd_check` must succeed before any other call is made on a document. The other
functions assume a well-formed document and do not re-report its errors.

Integers are read as `u32` or `i32` for now. X can hold 64 and 128-bit values,
but it cannot yet *return* one from a function (`CX_X_CORE_LANG.md` §2.1), and
the wide readers wait on that rather than working around it.

The reader keeps its error state in two globals, `xd_err` and `xd_err_at`. That
is fine for a single-threaded process, which is every process today; it will
need revisiting with threads.

---

## 6a. The DevKit reader, and the build check

`devkit/CXEX.Lang/Data/XData.cs` is a **line-for-line port** of
`os/std/xdata.xfxn`, not a second implementation. A build-time check is only
worth having if it accepts exactly what the supervisor accepts; a document that
passed the build and failed at boot would be worse than no check. Keeping the
two walks identical is what makes that agreement cheap to keep, and it is
enforced rather than hoped for: the DevKit's differential test (`tests/xdata/difftest.py`,
being ported to `CXEX.Tests/XData` under xUnit) runs
generated documents — valid, mutated, and nested past the depth limit —
through both readers, the X one compiled and run natively, and fails on any
difference in **error code or byte offset**. The error numbers are the same
`XD_E_*` values on both sides.

`cxk check-xdata <files> [--keys a,b,c]` exposes it. The CXK build runs it over
every `.xosv` before staging them, with `--keys` set to the keys the supervisor
knows, so a misspelt key or a missing comma stops the build with
`file:line:col` instead of appearing as a line in the boot log. The key list
lives in two places — `svc_keys_known()` in the supervisor and `SERVICE_XOSV`'s
check in `tools/cmake/CMakeLists.txt` — and each points at the other. A cxk
published before `check-xdata` existed is detected at configure time: the
build warns and skips the check rather than failing.

The build checks syntax and keys, not kinds: `start = 5` passes the build and is
refused by the supervisor at boot. Kinds belong to schemas (§7), which would
let both sides check them from one declaration.

---

## 6b. The kernel reads X Data with the X reader

The kernel does not have a C reader. It links `os/std/xdata.xfxn` directly:
the build compiles it with `cxk compile --object` (every function and global
exported, no entry point) and links the object with the kernel's C objects.
C calls it through `kernel/lib/format/xdata.h`, whose prototypes and
`struct xd_val` must match the X declarations by hand.

That works because X's calling convention is cdecl — arguments pushed right to
left, the caller pops, the result in `eax`, `ebx`/`esi`/`edi` preserved — and
because the reader imports nothing and allocates nothing. Its error position
lives in globals, so it is not reentrant; the kernel reads X Data once, at
boot, before anything else could call it.

The kernel's first document is `kernel.xkco` (`kernel/kconfig.c`), and the
self-tests check that it parses, and that every refusal points at the right
byte.

---

## 7. Intent — not built yet

- **Round-trip editing.** Every value the reader returns is a span of the
  original text. An editor changes a value by replacing its span and leaving
  everything else — comments, ordering, spacing — byte-for-byte as it was. That
  property is the single biggest reason people dislike JSON for configuration,
  it costs nothing to keep when every value is already a span, and it is
  impossible to add to a reader that builds a tree and throws the text away.
- **Schemas**, checking a document's keys, kinds and ranges against a
  declaration, so "strict types like Postgres" is a property of the file and not
  of each reader's diligence. Whether a schema is itself X Data is open.
- **Quoted keys**, wide integers, and nested block comments, when something
  needs them.
