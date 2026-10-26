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
    [InlineData("const A: u32 = 1\nsyscall A = 2\n", "declared twice")]
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
    [InlineData("syscall SYS_A = 1 fn a(p: *nothing) -> i32\n", "not a structure")]
    [InlineData("syscall SYS_A = 1 fn a(a: u32, b: u32, c: u32, d: u32, e: u32, f: u32) -> i32\n", "at most 5")]
    [InlineData("syscall SYS_A = 1 fn a() -> u64\n", "returns i32 or void")]
    [InlineData("syscall SYS_A = 1 fn a(x: u64) -> i32\n", "u32, i32 or a pointer")]
    [InlineData("syscall SYS_A = 1 fn a(x: u32, x: u32) -> i32\n", "declared twice")]
    [InlineData("syscall SYS_A = 1 fn a() -> i32\nsyscall SYS_B = 2 fn a() -> i32\n", "wrapper a is declared twice")]
    [InlineData("syscall SYS_A = 1 fn a(\n", "syscall NAME = number")]
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
            "syscall SYS_C = 3\n"));
        Assert.Contains("fn a(n: u32, h: i32, p: *s, b: *u8) -> i32 { return __syscall(SYS_A, n, h as u32, p as u32, b as u32, 0); }\n", x, StringComparison.Ordinal);
        Assert.Contains("fn b() -> void { __syscall(SYS_B, 0, 0, 0, 0, 0); }\n", x, StringComparison.Ordinal);
        Assert.DoesNotContain("SYS_C", x, StringComparison.Ordinal);   // no wrapper declared, none written
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_constant_reaches_the_model_in_file_order_with_its_type()
    {
        AbiModel m = XfilEmit.Model(Parse(
            "const A: u32 = 1\nsyscall SYS_X = 0x10\nenum e {\n    E0 = 0\n    E1 = 1\n}\nconst NEG: i32 = -1\n"));
        Assert.Equal(new[] { ("A", 1L, "u32"), ("SYS_X", 16L, "u32"), ("E0", 0L, "u32"), ("E1", 1L, "u32"), ("NEG", -1L, "i32") },
                     m.Constants.Select(c => (c.Name, c.Value, c.XType)));
    }
}
