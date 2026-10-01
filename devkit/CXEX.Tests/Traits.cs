namespace CXEX.Tests;

/// <summary>
/// The four test categories of HARDENING_PLAN D1, as trait values.
///
/// <para>They exist so a run can be selected by what it needs, not by what it is
/// called: <c>dotnet test --filter "Category=Unit|Category=Adversarial"</c> is the
/// set that needs nothing but .NET, and is the set CI runs on every push.</para>
/// </summary>
public static class Categories
{
    /// <summary>The trait name every category is filed under.</summary>
    public const string Key = "Category";

    /// <summary>Direct tests of a library. Needs nothing but .NET.</summary>
    public const string Unit = "Unit";

    /// <summary>Malformed input: ELF, CXEX, crypto, files and tooling. Needs nothing but .NET.</summary>
    public const string Adversarial = "Adversarial";

    /// <summary>
    /// Needs clang and ld.lld (D5), and a Linux host or WSL — these run compiled X
    /// natively, and X's host platform makes Linux system calls.
    /// </summary>
    public const string Toolchain = "Toolchain";

    /// <summary>
    /// Compares the C# implementation against the X one in this repository. Needs
    /// everything Toolchain needs, plus the OS sources.
    /// </summary>
    public const string Differential = "Differential";
}
