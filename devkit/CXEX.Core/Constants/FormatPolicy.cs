// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
namespace CXEX.Core.Constants;

/// <summary>The on-disk and on-wire formats CXOS defines, each with its own
/// monotonic version (see <c>versions.json</c>).</summary>
public enum CXFormat
{
    /// <summary>The CXEX executable container: 56-byte header, 28-byte section entries.</summary>
    Cxex,
    /// <summary>The syscall and capability contract in <c>abi/cxk_abi.h</c>.</summary>
    Abi,
    /// <summary>The boot-information block stage 2 hands the kernel.</summary>
    Cxbi,
    /// <summary>The CXFS filesystem on-disk layout.</summary>
    Cxfs,
    /// <summary>The XBPT partition table.</summary>
    Xbpt,
    /// <summary>The XSTG install manifest.</summary>
    Xstg,
    /// <summary>The CXPK public key container.</summary>
    Xkpk,
}

/// <summary>
/// The one place the DevKit decides what it can read, write and target
/// (DevKit engineering review §13).
///
/// <para>The review's concern is scattering: "as CXEX, X data, ABI structures
/// and toolchain metadata gain versions, avoid scattering compatibility checks
/// across unrelated commands". They had scattered, if not far - each type
/// carried its own private constant (<c>CXEXExecutable.SupportedFormatVersion</c>,
/// <c>CXEXMemoryLayout.FormatVersion</c>, <c>CXKeyGenerator.XKPK_VERSION</c>,
/// each a literal <c>1</c>), so the read side and the write side agreed by
/// coincidence rather than by construction and a bump meant finding every
/// one.</para>
///
/// <para><b>This is a policy, not a registry.</b> <c>versions.json</c> remains
/// the authority on what the current version of each format *is*, and
/// <c>cxk check-versions</c> enforces that the files naming a version agree
/// with it. What that file cannot express is the question a reader actually
/// asks - *can I handle this?* - which is a range, not a number. A reader may
/// legitimately be older than the file it is handed; that asymmetry is the
/// reason the formats are versioned separately from the components at all.
/// The format-policy tests compare the two so they cannot
/// drift.</para>
///
/// <para>Answers the five questions §13 lists: whether a format version is
/// supported, whether this DevKit can read it, whether it can write it,
/// whether an ABI version is compatible, and what to say when the answer is
/// no.</para>
/// </summary>
public static class FormatPolicy
{
    /// <summary>What this DevKit accepts and produces for one format.</summary>
    public readonly record struct Support(int Min, int Max, int Writes)
    {
        public bool CanRead(int version) => version >= Min && version <= Max;
        public bool CanWrite(int version) => version == Writes;
    }

    /// <summary>
    /// The supported range per format. <c>Min</c>/<c>Max</c> is what can be
    /// READ; <c>Writes</c> is the single version produced - a writer that can
    /// emit several versions of a format is a compatibility burden nothing has
    /// asked for yet, so emitting exactly one is deliberate.
    ///
    /// <para>Where Min == Max there is only ever one version in existence. A
    /// format that has been bumped keeps its old version readable only if the
    /// reader genuinely still handles it: CXFS is at 2 and the DevKit does not
    /// read v1, so its range is 2..2 rather than 1..2. Claiming a range wider
    /// than the code supports is worse than claiming none.</para>
    /// </summary>
    private static readonly Dictionary<CXFormat, Support> Table = new()
    {
        [CXFormat.Cxex] = new(Min: 1, Max: 1, Writes: 1),
        [CXFormat.Abi]  = new(Min: 2, Max: 2, Writes: 2),
        [CXFormat.Cxbi] = new(Min: 1, Max: 1, Writes: 1),
        [CXFormat.Cxfs] = new(Min: 2, Max: 2, Writes: 2),
        [CXFormat.Xbpt] = new(Min: 1, Max: 1, Writes: 1),
        [CXFormat.Xstg] = new(Min: 2, Max: 2, Writes: 2),
        [CXFormat.Xkpk] = new(Min: 1, Max: 1, Writes: 1),
    };

    public static Support Of(CXFormat format) => Table[format];

    /// <summary>Every format this policy covers, for tests and for `cxk inspect`.</summary>
    public static IEnumerable<CXFormat> All => Table.Keys;

    /// <summary>"Is this artifact format supported?" / "Can this DevKit read this version?"</summary>
    public static bool CanRead(CXFormat format, int version) => Table[format].CanRead(version);

    /// <summary>"Can this toolchain produce the requested target format?"</summary>
    public static bool CanWrite(CXFormat format, int version) => Table[format].CanWrite(version);

    /// <summary>The version this DevKit emits for a format.</summary>
    public static int WriteVersion(CXFormat format) => Table[format].Writes;

    /// <summary>
    /// "Is an ABI version compatible?" Kept as its own question because the
    /// ABI is not a container the DevKit parses - it is a contract stamped
    /// into every image so a kernel can refuse one built against a different
    /// one. The DevKit's job is to stamp the right number and to refuse to
    /// pretend otherwise.
    /// </summary>
    public static bool AbiCompatible(int abiVersion) => CanRead(CXFormat.Abi, abiVersion);

    /// <summary>
    /// One sentence saying why a version was refused, naming the version and
    /// the range rather than just declining - a refusal that does not say what
    /// would have been accepted sends the reader to the source.
    /// </summary>
    public static string ExplainRead(CXFormat format, int version)
    {
        var s = Table[format];
        if (s.CanRead(version)) return $"{format} version {version} is supported.";
        return s.Min == s.Max
            ? $"{format} version {version} is not supported (this DevKit reads version {s.Min})."
            : $"{format} version {version} is not supported (this DevKit reads {s.Min} through {s.Max}).";
    }
}
