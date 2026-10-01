using CXEX.Lang.Abi;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// <c>AbiPrelude.Generate()</c> is a hand-maintained copy of <c>abi/cxk_abi.h</c>
/// that lives in a different language from the header it mirrors, and nothing
/// generates it. When the two drift, the userland stops compiling with "undefined
/// name" errors a long way from the cause — which has already happened once, when
/// the kernel gained <c>SYS_MOUSE_READ</c> and <c>struct mouse_state</c> and
/// <c>gui.xfxn</c> used both.
///
/// <para><c>cxk check-abi</c> guards this at build time. This makes
/// <c>dotnet test</c> the gate too (DevKit design doc §5.2, Q-F), so the drift is
/// caught without running an OS build.</para>
/// </summary>
public class AbiSyncTests
{
    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Prelude_matches_the_real_header()
    {
        string root = Requires.Repo();
        string headerPath = Path.Combine(root, "abi", "cxk_abi.h");
        Assert.True(File.Exists(headerPath), $"ABI header not found at {headerPath}");

        AbiSyncReport report = AbiSync.Compare(File.ReadAllText(headerPath), AbiPrelude.Generate());

        // Report every finding, not just the count: a bare "3 errors" sends whoever
        // reads the failure back to the CLI to find out which three.
        string detail = string.Join(
            Environment.NewLine,
            report.Findings
                  .Where(f => f.Kind == AbiFindingKind.Error)
                  .Select(f => "  " + f.Message));

        Assert.True(
            report.Count(AbiFindingKind.Error) == 0,
            $"the X ABI prelude has drifted from abi/cxk_abi.h:{Environment.NewLine}{detail}{Environment.NewLine}" +
            "Fix AbiPrelude.Generate() in CXEX.Lang/Abi/AbiPrelude.cs, then re-run.");
    }

    /// <summary>
    /// The comparison must actually be comparing something. A prelude that parsed
    /// to nothing would produce a clean report for every header on earth, and the
    /// test above would pass while checking nothing at all — the same failure mode
    /// SABOTAGE exists to catch in the differential suites.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Comparison_is_not_vacuous()
    {
        string root = Requires.Repo();
        AbiSyncReport report = AbiSync.Compare(
            File.ReadAllText(Path.Combine(root, "abi", "cxk_abi.h")),
            AbiPrelude.Generate());

        Assert.True(report.HeaderConstants > 0, "parsed no constants out of cxk_abi.h");
        Assert.True(report.PreludeConstants > 0, "parsed no constants out of the X prelude");
        Assert.True(report.HeaderStructs > 0, "parsed no structs out of cxk_abi.h");
        Assert.True(report.PreludeStructs > 0, "parsed no structs out of the X prelude");
    }

    /// <summary>
    /// The planted-difference test for <c>AbiSync</c> itself: a prelude with one
    /// constant given the wrong value must be reported as an error. Without this,
    /// nothing proves <see cref="AbiSync.Compare"/> can fail at all.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_planted_value_mismatch_is_reported()
    {
        const string header = """
            #define SYS_WRITE 1
            #define SYS_EXIT  2
            """;
        const string preludeGood = """
            const SYS_WRITE: i32 = 1;
            const SYS_EXIT: i32 = 2;
            """;
        const string preludeBad = """
            const SYS_WRITE: i32 = 1;
            const SYS_EXIT: i32 = 99;
            """;

        Assert.Equal(0, AbiSync.Compare(header, preludeGood).Count(AbiFindingKind.Error));

        AbiSyncReport bad = AbiSync.Compare(header, preludeBad);
        Assert.True(bad.Count(AbiFindingKind.Error) > 0,
            "AbiSync reported no error for a prelude whose SYS_EXIT disagrees with the header");
    }
}
