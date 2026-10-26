// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text.RegularExpressions;
using CXEX.Lang.Abi;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The ABI generator (DevKit engineering review §7). Since roadmap step 6a1 the
/// one source is <c>abi/kernel.xfil</c>: <c>abi/cxk_abi.h</c> and X's view of the ABI
/// are both generated from it (<see cref="XfilEmit"/>). The C header is still read
/// back with <see cref="AbiModel.Parse"/>, which holds the two to each other.
///
/// <para>When the .xfil replaced the hand-written header (6a1), the header it
/// generated matched the old one in every constant, structure and field type
/// (check-abi's comparison, run against the old header), and every kernel and
/// program built from it, both kernels included, came out byte-identical.</para>
///
/// <para>When it replaced the hand-kept prelude, the generated declarations were
/// checked against the old ones: all 95 constants and 12 structures matched in
/// name, value and type. The whole OS was then rebuilt with each and every
/// package came out byte-identical. These tests keep it that way. Nothing in the
/// header may be dropped on the way to X, the hand-written part may not declare
/// what is generated, and a wrong field type has to be caught, which names and
/// order alone never did.</para>
/// </summary>
public class AbiGeneratorTests
{
    private static string DiskHeader() =>
        File.ReadAllText(Path.Combine(Requires.Repo(), "abi", "cxk_abi.h"));

    private static string DiskXfil() =>
        File.ReadAllText(Path.Combine(Requires.Repo(), "abi", "kernel.xfil"));

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_built_in_interface_is_the_one_on_disk()
    {
        // The DevKit embeds kernel.xfil at build time. If these differ, the test
        // binary predates an interface change - the same staleness check-abi
        // reports for a stale tools/cxk.exe.
        Assert.Equal(DiskXfil().Replace("\r\n", "\n"), AbiModel.EmbeddedXfil.Replace("\r\n", "\n"));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_committed_header_is_what_the_interface_generates()
    {
        // A hand edit to the header, or an interface change committed without
        // `cxk abi generate`, fails here and in check-abi.
        Assert.Equal(XfilEmit.CHeader(XfilParser.Parse(DiskXfil())), DiskHeader().Replace("\r\n", "\n"));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_header_read_back_is_the_interface()
    {
        // What C compiles and what X is given agree: every constant by name,
        // value and type, every structure field by name, order and type.
        AbiModel fromC = AbiModel.Parse(DiskHeader());
        AbiModel fromXfil = XfilEmit.Model(XfilParser.Parse(DiskXfil()));
        Assert.Equal(fromXfil.Constants.Select(c => (c.Name, c.Value, c.XType)),
                     fromC.Constants.Select(c => (c.Name, c.Value, c.XType)));
        Assert.Equal(fromXfil.Structs.Select(s => (s.Name, string.Join(",", s.Fields.Select(f => f.Name + ":" + f.XType)))),
                     fromC.Structs.Select(s => (s.Name, string.Join(",", s.Fields.Select(f => f.Name + ":" + f.XType)))));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_header_constant_and_structure_reaches_the_prelude_exactly_once()
    {
        var model = AbiModel.Parse(DiskHeader());
        string prelude = AbiPrelude.Generate();

        Assert.NotEmpty(model.Constants);
        Assert.NotEmpty(model.Structs);

        foreach (var c in model.Constants)
        {
            var decls = Regex.Matches(prelude, $@"^const\s+{c.Name}\s*:\s*(\w+)\s*=\s*([^;]+);", RegexOptions.Multiline);
            var one = Assert.Single(decls);   // once: the hand-written part declares none of its own
            Assert.Equal(c.XType, one.Groups[1].Value);
            string v = one.Groups[2].Value.Trim();
            long value = v.StartsWith("0x", StringComparison.Ordinal) ? Convert.ToInt64(v[2..], 16) : long.Parse(v);
            Assert.Equal(c.Value, value);
        }

        foreach (var s in model.Structs)
            Assert.Single(Regex.Matches(prelude, $@"^struct\s+{s.Name}\s*\{{", RegexOptions.Multiline));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_whole_header_is_read_including_families_the_old_prelude_left_out()
    {
        var model = AbiModel.Parse(DiskHeader());

        // NET_OP_* was the family the hand-kept prelude never mirrored, so
        // os/std/net.xfxn carried a third copy. The four unfamilied constants were
        // never compared at all.
        Assert.Equal(7, model.Constants.Count(c => c.Name.StartsWith("NET_OP_", StringComparison.Ordinal)));
        foreach (string name in new[] { "CXK_ABI_VERSION", "USER_ARGS_BASE", "USER_ARGS_MAX", "USER_ARGS_MAXC" })
            Assert.NotNull(model.Constant(name));

        // An OR-expression of earlier constants is evaluated, not skipped.
        long expected = 0;
        foreach (string g in new[] { "GRANT_CONSOLE", "GRANT_MEM", "GRANT_DISK", "GRANT_SPAWN", "GRANT_POWER",
                                     "GRANT_ENDPOINT", "GRANT_FRAMEBUFFER", "GRANT_NET" })
            expected |= model.Constant(g)!.Value;
        Assert.Equal(expected, model.Constant("GRANT_OS_BASELINE")!.Value);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void C_types_map_to_the_x_types_the_kernel_layout_needs()
    {
        var model = AbiModel.Parse(DiskHeader());
        AbiStruct S(string n) => Assert.Single(model.Structs, s => s.Name == n);
        string T(string s, string f) => Assert.Single(S(s).Fields, x => x.Name == f).XType;

        Assert.Equal("u32", T("spawn_args", "image"));        // a user address, uint32_t until step 6
        Assert.Equal("i32", T("spawn_args", "broker_endpoint")); // int
        Assert.Equal("u32", T("ipc_recv_args", "sender"));
        Assert.Equal("[64]u8", T("file_stat", "name"));       // char[FILE_NAME_MAX]
        Assert.Equal(new[] { "x", "y", "buttons", "seq" },    // int32_t x, y; - one declaration, two fields
                     S("mouse_state").Fields.Select(f => f.Name));
        Assert.Equal("i32", T("mouse_state", "y"));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void C_pointer_types_map_to_x_pointers()
    {
        // The disk header has no pointer field today (user addresses are
        // uint32_t until step 6 widens them), so the mapping is held here.
        var model = AbiModel.Parse("struct p { const void *a; int *b; uint32_t *c; char *d; };\n");
        string T(string f) => Assert.Single(Assert.Single(model.Structs).Fields, x => x.Name == f).XType;
        Assert.Equal("*u8", T("a"));
        Assert.Equal("*i32", T("b"));
        Assert.Equal("*u32", T("c"));
        Assert.Equal("*u8", T("d"));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_wrong_field_type_in_the_prelude_is_an_error()
    {
        // Same name, same position, wrong width: every later field two bytes adrift.
        // Before 2026-10-16 check-abi compared names and order only and passed this.
        string prelude = AbiPrelude.Generate();
        string broken = Regex.Replace(prelude, @"(struct mouse_state \{[^}]*buttons: )u32", "${1}u16");
        Assert.NotEqual(prelude, broken);

        var report = AbiSync.Compare(DiskHeader(), broken);
        Assert.Contains(report.Findings, f => f.Kind == AbiFindingKind.Error &&
                                              f.Message.Contains("mouse_state", StringComparison.Ordinal) &&
                                              f.Message.Contains("buttons", StringComparison.Ordinal) &&
                                              f.Message.Contains("type", StringComparison.Ordinal));

        // And the real prelude against the real header has no errors and no
        // warnings. A second, looser header parser in AbiSync once reported
        // GRANT_OS_BASELINE (an OR-expression) as missing from the header.
        Assert.DoesNotContain(AbiSync.Compare(DiskHeader(), prelude).Findings, f => f.Kind != AbiFindingKind.Info);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_devkit_statement_of_the_abi_version_is_the_models()
    {
        // The images CXEXLayoutEngine stamps, and what FormatPolicy says the DevKit
        // writes and accepts, both restate CXK_ABI_VERSION. They are C# compile-time
        // constants, so they cannot read the model; this holds them to it instead.
        long abi = AbiModel.Parse(DiskHeader()).Constant("CXK_ABI_VERSION")!.Value;
        Assert.Equal(abi, CXEX.Build.Engines.CXEXLayoutEngine.AbiVersion);
        Assert.Equal(abi, CXEX.Core.Constants.FormatPolicy.WriteVersion(CXEX.Core.Constants.CXFormat.Abi));
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("#define ODD (1 << 3)\n")]                        // an operator it does not evaluate
    [InlineData("#define LATER (UNDEFINED_YET | 1)\n")]           // a name it has not seen
    [InlineData("struct f { float x; };\n")]                      // a C type with no X mapping
    public void A_header_construct_the_generator_cannot_express_is_refused_not_dropped(string header)
    {
        Assert.Throws<InvalidDataException>(() => AbiModel.Parse(header));
    }
}
