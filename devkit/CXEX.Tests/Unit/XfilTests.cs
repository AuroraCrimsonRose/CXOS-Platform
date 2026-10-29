// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Lang.Abi;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The <c>.xfil</c> parser and what it generates (roadmap step 6a1,
/// <c>docs/kernel/CX_SYSCALLS_IPC.md</c> §9). The parser is where an interface is
/// made safe to generate from, so each of its refusals is held here: a file it
/// accepted wrongly would put a padded structure, an out-of-range constant or a
/// wrapper over an unknown type into the kernel's header and every program.
/// </summary>
public class XfilTests
{
    private const string Head = "xfil 1\nlibrary cx.kernel\ndomain kernel\n";

    private static XfilFile Parse(string body) => XfilParser.Parse(Head + body);

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_abi_hash_covers_the_definition_and_not_its_comments()
    {
        const string body = "/// a heading\n\nconst A: u32 = 1   /// note\nstruct s {\n    x: u32\n}\n";
        byte[] hash = Parse(body).Hash;
        Assert.Equal(32, hash.Length);

        // Rewording, re-indenting, adding a comment or a blank line: same hash,
        // so programs built before the edit still load.
        Assert.Equal(hash, Parse("/// another heading\n// a comment\n\nconst  A: u32 = 1   /// other note\n\nstruct s {\n\tx: u32\n}\n\n").Hash);
        Assert.Equal(hash, XfilParser.Parse((Head + body).Replace("\n", "\r\n")).Hash);

        // Any change to a name, value, type or field order: a different hash.
        Assert.NotEqual(hash, Parse(body.Replace("= 1", "= 2")).Hash);
        Assert.NotEqual(hash, Parse(body.Replace("const A", "const B")).Hash);
        Assert.NotEqual(hash, Parse(body.Replace("x: u32", "x: i32")).Hash);
        Assert.NotEqual(hash, Parse(body.Replace("    x: u32\n", "    x: u32\n    y: u32\n")).Hash);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("xfil 2\nlibrary cx.kernel\ndomain kernel\n", "xfil 1")]                       // a version it cannot read
    [InlineData("const A: u32 = 1\n", "expected 'xfil")]                                          // no header
    [InlineData("xfil 1\nlibrary cx.display\ndomain kernel\n", "exactly one library")]           // a second kernel library
    [InlineData("xfil 1\nlibrary cx.kernel\ndomain driver\n", "not a domain")]
    [InlineData("xfil 1\nlibrary Kernel\ndomain kernel\n", "reverse-dotted")]
    [InlineData("/// doc\nxfil 1\nlibrary cx.kernel\ndomain kernel\n", "header lines")]
    public void A_bad_header_is_refused(string text, string message)
    {
        var e = Assert.Throws<XfilException>(() => XfilParser.Parse(text));
        Assert.Contains(message, e.Message, StringComparison.Ordinal);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("const A: u32 = 1\nconst A: u32 = 1\n", "declared twice")]
    [InlineData("const A: u32 = 1\nenum e {\n    A = 2\n}\n", "declared twice")]              // one namespace for every constant
    [InlineData("const A: u32 = 1\nsyscall A = 2 () -> i32\n", "declared twice")]
    [InlineData("const A: u32 = 0x100000000\n", "does not fit in u32")]
    [InlineData("const A: u32 = -1\n", "does not fit in u32")]
    [InlineData("const A: i32 = 0x80000000\n", "does not fit in i32")]
    [InlineData("const A: u64 = 1\n", "u32 or i32")]
    [InlineData("const A: u32 = B | 1\n", "cannot evaluate 'B'")]                                 // only earlier constants
    [InlineData("const A: u32 = 1 << 3\n", "cannot evaluate")]                                    // only OR
    [InlineData("const A: u32 = -1 | 2\n", "cannot be ORed")]
    [InlineData("enum e {\n}\n", "no members")]
    [InlineData("enum e {\n    A\n}\n", "NAME = value")]                                          // every value written out
    [InlineData("enum e {\n    A = 0\n", "closing")]
    [InlineData("frobnicate x\n", "expected const, enum, struct or syscall")]
    public void A_bad_constant_is_refused(string body, string message)
    {
        var e = Assert.Throws<XfilException>(() => Parse(body));
        Assert.Contains(message, e.Message, StringComparison.Ordinal);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("struct s {\n    a: u8\n    b: u32\n}\n", "offset 1")]                            // padding the compiler would insert
    [InlineData("struct s {\n    a: u32\n    b: u8\n}\n", "pad at the end")]                       // tail padding
    [InlineData("struct s {\n    a: u16\n    b: u64\n}\n", "offset 2")]
    [InlineData("struct s {\n    p: *u8\n}\n", "holds no pointers")]                               // §5: addresses are integers
    [InlineData("struct s {\n    a: f32\n}\n", "unknown type")]
    [InlineData("struct s {\n    a: u32\n    a: u32\n}\n", "declared twice")]
    [InlineData("struct s {\n}\n", "no fields")]
    [InlineData("struct s {\n    a: u32\n}\nstruct s {\n    b: u32\n}\n", "declared twice")]
    [InlineData("struct s {\n    a: char[N]\n}\n", "not a number or an earlier constant")]
    [InlineData("struct s {\n    a: char[0]\n}\n", "1..65536")]
    [InlineData("struct s {\n    a: f32[4]\n}\n", "char or a fixed-width integer")]
    [InlineData("struct s {\n    a: u32\n    /// documents nothing\n}\n", "documents nothing")]
    public void A_bad_structure_is_refused(string body, string message)
    {
        var e = Assert.Throws<XfilException>(() => Parse(body));
        Assert.Contains(message, e.Message, StringComparison.Ordinal);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("syscall SYS_A = 1 fn a(p: *nothing) -> i32\n", "neither an integer type nor a structure")]
    [InlineData("syscall SYS_A = 1 fn a(a: u32, b: u32, c: u32, d: u32, e: u32, f: u32) -> i32\n", "at most 5")]
    [InlineData("syscall SYS_A = 1 fn a() -> u64\n", "returns i32 or void")]
    [InlineData("syscall SYS_A = 1 fn a(x: u8) -> i32\n", "u32, i32, u64, i64 or a pointer")]
    [InlineData("syscall SYS_A = 1 fn a(a: u64, b: u64, c: i64) -> i32\n", "needs 6 argument registers on x86-32")]   // a pair each
    [InlineData("syscall SYS_A = 1 fn a(a: u32, b: u32, c: u32, d: u32, e: u64) -> i32\n", "needs 6 argument registers")]
    [InlineData("syscall SYS_A = 1 fn a(x: u32, x: u32) -> i32\n", "declared twice")]
    [InlineData("syscall SYS_A = 1 fn a() -> i32\nsyscall SYS_B = 2 fn a() -> i32\n", "wrapper a is declared twice")]
    [InlineData("syscall SYS_A = 1 fn a(\n", "syscall NAME = number")]
    [InlineData("syscall SYS_A = 1\n", "every syscall declares its arguments")]                  // the table could not decode it
    [InlineData("syscall SYS_A = 1 (x: u8) -> i32\n", "u32, i32, u64, i64 or a pointer")]            // no wrapper, still checked
    [InlineData("syscall SYS_A = 1 () -> i32\nsyscall SYS_B = 1 () -> i32\n", "which is SYS_A's")]  // one table slot, two handlers
    [InlineData("syscall SYS_A = 0x400 () -> i32\n", "up to 1023")]                               // a table that size is a mistake
    [InlineData("syscall SYSA = 1 () -> i32\n", "starts with SYS_")]                              // its handler is named after the rest
    [InlineData("syscall SYS_ = 1 () -> i32\n", "starts with SYS_")]
    public void A_bad_syscall_is_refused(string body, string message)
    {
        var e = Assert.Throws<XfilException>(() => Parse(body));
        Assert.Contains(message, e.Message, StringComparison.Ordinal);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("/// ends a comment */\nconst A: u32 = 1\n")]
    [InlineData("const A: u32 = 1   /// opens one /*\n")]
    public void Documentation_that_would_break_a_c_comment_is_refused(string body)
    {
        var e = Assert.Throws<XfilException>(() => Parse(body));
        Assert.Contains("carried into C comments", e.Message, StringComparison.Ordinal);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_refusal_names_its_line()
    {
        var e = Assert.Throws<XfilException>(() => Parse("// comment\n\nconst A: u32 = 1\nconst A: u32 = 2\n"));
        Assert.Equal(7, e.Line);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_layout_is_computed_and_asserted_in_the_header()
    {
        var f = Parse("const W: u32 = 2\nstruct s {\n    a: u32\n    b: u16\n    c: char[W]\n    d: u64\n}\n");
        var s = Assert.Single(f.Structs);
        Assert.Equal(new[] { 0, 4, 6, 8 }, s.Fields.Select(x => x.Offset));
        Assert.Equal(16, s.Size);
        Assert.Contains("CXK_ABI_STATIC_ASSERT(sizeof(struct s) == 16, \"ABI layout: s\");", XfilEmit.CHeader(f), StringComparison.Ordinal);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_header_keeps_values_as_written()
    {
        string h = XfilEmit.CHeader(Parse(
            "const A: u32 = 0x10u\nconst B: u32 = 2\nconst AB: u32 = A | B\nconst E: i32 = -3\n" +
            "const N: u32 = 8\nstruct t {\n    name: char[N]\n    x: i32\n}\n"));
        Assert.Matches(@"#define A\s+0x10u\n", h);                 // the suffix is the kernel's signedness
        Assert.Matches(@"#define AB\s+\(A \| B\)\n", h);
        Assert.Matches(@"#define E\s+\(-3\)\n", h);
        Assert.Contains("    char name[N];\n", h, StringComparison.Ordinal);
        Assert.Contains("    int32_t x;\n", h, StringComparison.Ordinal);
        Assert.Contains("CXK_ABI_STATIC_ASSERT(sizeof(struct t) == 12, \"ABI layout: t\");", h, StringComparison.Ordinal);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Documentation_above_an_item_is_its_own_and_a_block_before_a_blank_line_stands_alone()
    {
        var f = Parse("/// a heading\n\n/// about A\nconst A: u32 = 1   /// trailing\nconst B: u32 = 2\n");
        Assert.IsType<XfilNote>(f.Items[0]);
        Assert.Equal(new[] { "a heading" }, f.Items[0].Doc.Lines);
        var a = Assert.IsType<XfilConst>(f.Items[1]);
        Assert.Equal(new[] { "about A" }, a.Doc.Lines);
        Assert.Equal("trailing", a.Doc.Trailing);
        Assert.Empty(f.Items[2].Doc.Lines);

        string h = XfilEmit.CHeader(f);
        Assert.Contains("/* a heading */", h, StringComparison.Ordinal);
        Assert.Matches(@"/\* about A \*/\n#define A\s+1\s+/\* trailing \*/\n", h);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_wrapper_passes_each_argument_as_a_register_and_pads_to_five()
    {
        string x = XfilEmit.XWrappers(Parse(
            "struct s {\n    a: u32\n}\n" +
            "syscall SYS_A = 1 fn a(n: u32, h: i32, p: *s, b: *u8) -> i32\n" +
            "syscall SYS_B = 2 fn b() -> void\n" +
            "syscall SYS_C = 3 (op: u32) -> i32\n"));
        Assert.Contains("fn a(n: u32, h: i32, p: *s, b: *u8) -> i32 { return __syscall(SYS_A, n, h as u32, p as u32, b as u32, 0); }\n", x, StringComparison.Ordinal);
        Assert.Contains("fn b() -> void { __syscall(SYS_B, 0, 0, 0, 0, 0); }\n", x, StringComparison.Ordinal);
        Assert.DoesNotContain("SYS_C", x, StringComparison.Ordinal);   // no wrapper declared, none written
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void On_x86_64_a_wrapper_passes_a_pointer_whole()
    {
        string x = XfilEmit.XWrappers(Parse(
            "struct s {\n    a: u32\n}\n" +
            "syscall SYS_A = 1 fn a(n: u32, h: i32, p: *s, b: *u8) -> i32\n"), pairs: false);
        // `p as u32` would cut a 64-bit address to its low half
        Assert.Contains("fn a(n: u32, h: i32, p: *s, b: *u8) -> i32 { return __syscall(SYS_A, n, h as u32, p, b, 0); }\n", x, StringComparison.Ordinal);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_syscall_gets_a_handler_prototype_a_decoder_and_a_table_slot()
    {
        XfilFile f = Parse(
            "struct s {\n    a: u32\n}\n" +
            "syscall SYS_FILE_OP = 0x10 fn file_op(n: u32, h: i32, p: *s, b: *u32, last: i32) -> i32\n" +
            "syscall SYS_EXIT = 2 fn exit(code: i32) -> void\n" +
            "syscall SYS_POWER = 0x05 () -> i32\n");
        string h = XfilEmit.CTableHeader(f), c = XfilEmit.CTable(f);

        // the handler is named after the syscall, not its wrapper; pointers are user addresses
        Assert.Contains("int syscall_file_op(uint32_t n, int32_t h, uint32_t p, uint32_t b, int32_t last);", h, StringComparison.Ordinal);
        Assert.Contains("void syscall_exit(int32_t code);", h, StringComparison.Ordinal);
        Assert.Contains("int syscall_power(void);", h, StringComparison.Ordinal);   // no wrapper, still a handler
        Assert.Contains("#define SYSCALL_TABLE_SIZE      17\n", h, StringComparison.Ordinal);
        Assert.Contains("#define SYSCALL_COUNT           3\n", h, StringComparison.Ordinal);

        // each register to its parameter in order, i32 converted; void answers 0
        Assert.Contains("static int decode_file_op(const uintptr_t *a) { return syscall_file_op((uint32_t)a[0], (int32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (int32_t)a[4]); }", c, StringComparison.Ordinal);
        Assert.Contains("static int decode_exit(const uintptr_t *a) { syscall_exit((int32_t)a[0]); return 0; }", c, StringComparison.Ordinal);
        Assert.Contains("static int decode_power(const uintptr_t *a) { (void)a; return syscall_power(); }", c, StringComparison.Ordinal);

        // slots by number, in number order
        Assert.Contains("    [SYS_EXIT] = decode_exit,\n    [SYS_POWER] = decode_power,\n    [SYS_FILE_OP] = decode_file_op,\n};", c, StringComparison.Ordinal);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_u64_argument_is_a_register_pair_on_x86_32_and_one_register_on_x86_64()
    {
        XfilFile f = Parse(
            "syscall SYS_SEEK = 1 fn seek(h: i32, off: i64, origin: u32, pos: *u64) -> i32\n" +
            "syscall SYS_RANGE = 2 fn range(h: i32, start: u64, len: u64) -> i32\n");
        string h = XfilEmit.CTableHeader(f), c = XfilEmit.CTable(f), x = XfilEmit.XWrappers(f);

        // the handler takes the whole value; the register width decides how it arrives
        Assert.Contains("int syscall_seek(int32_t h, int64_t off, uint32_t origin, uint32_t pos);", h, StringComparison.Ordinal);
        Assert.Contains("#if UINTPTR_MAX == 0xFFFFFFFFu\n#define SYSCALL_PAIRS           1\n#else\n#define SYSCALL_PAIRS           0\n#endif\n", h, StringComparison.Ordinal);

        // x86-32 joins (low, high) and every later argument moves along one; x86-64 takes it whole
        Assert.Contains(
            "static int decode_seek(const uintptr_t *a) {\n#if SYSCALL_PAIRS\n" +
            "    return syscall_seek((int32_t)a[0], (int64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32), (uint32_t)a[3], (uint32_t)a[4]);\n" +
            "#else\n    return syscall_seek((int32_t)a[0], (int64_t)a[1], (uint32_t)a[2], (uint32_t)a[3]);\n#endif\n}\n",
            c, StringComparison.Ordinal);
        Assert.Contains("syscall_range((int32_t)a[0], (uint64_t)((uint64_t)a[1] | (uint64_t)a[2] << 32), (uint64_t)((uint64_t)a[3] | (uint64_t)a[4] << 32));", c, StringComparison.Ordinal);
        Assert.Contains("syscall_range((int32_t)a[0], (uint64_t)a[1], (uint64_t)a[2]);", c, StringComparison.Ordinal);

        // the X wrapper (x86-32) splits it low first, in the order the decoder joins
        Assert.Contains("return __syscall(SYS_SEEK, h as u32, off as u32, (off >> 32) as u32, origin, pos as u32); }\n", x, StringComparison.Ordinal);
        Assert.Contains("return __syscall(SYS_RANGE, h as u32, start as u32, (start >> 32) as u32, len as u32, (len >> 32) as u32); }\n", x, StringComparison.Ordinal);

        // the X wrapper (x86-64) passes it in one register, where SYSCALL_PAIRS 0 reads it
        string x64 = XfilEmit.XWrappers(f, pairs: false);
        Assert.Contains("return __syscall(SYS_SEEK, h as u32, off, origin, pos, 0); }\n", x64, StringComparison.Ordinal);
        Assert.Contains("return __syscall(SYS_RANGE, h as u32, start, len, 0, 0); }\n", x64, StringComparison.Ordinal);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_constant_reaches_the_model_in_file_order_with_its_type()
    {
        AbiModel m = XfilEmit.Model(Parse(
            "const A: u32 = 1\nsyscall SYS_X = 0x10 () -> void\nenum e {\n    E0 = 0\n    E1 = 1\n}\nconst NEG: i32 = -1\n"));
        Assert.Equal(new[] { ("A", 1L, "u32"), ("SYS_X", 16L, "u32"), ("E0", 0L, "u32"), ("E1", 1L, "u32"), ("NEG", -1L, "i32") },
                     m.Constants.Select(c => (c.Name, c.Value, c.XType)));
    }

    // The compilers call stub n at base + (n << SyscallStubShift); the kernel lays
    // the stubs SYSPAGE_STUB_SIZE apart. The two are written in different places,
    // so this is what keeps them one number (os/xc/x64.xfxn has the same shift).
    [Fact]
    public void The_emitters_stub_stride_is_the_ABIs()
    {
        AbiConstant c = AbiModel.Embedded.Constants.Single(k => k.Name == "SYSPAGE_STUB_SIZE");
        Assert.Equal(c.Value, 1L << CXEX.Lang.CodeGen.X64Emitter.SyscallStubShift);
    }
}
