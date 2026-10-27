// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
namespace CXEX.Lang.Abi;

/// <summary>
/// Documentation carried from a <c>.xfil</c> file into what it generates: the
/// <c>///</c> lines before an item (<see cref="Lines"/>) and the one after it on
/// the same line (<see cref="Trailing"/>).
/// </summary>
public sealed record XfilDoc(IReadOnlyList<string> Lines, string? Trailing)
{
    public static readonly XfilDoc None = new(Array.Empty<string>(), null);
}

/// <summary>A type an ABI value or field has. Widths are fixed: the ABI means the same to C, X and C#.</summary>
public abstract record XfilType
{
    /// <summary>Size in bytes.</summary>
    public abstract int Size { get; }
    /// <summary>Natural alignment in bytes.</summary>
    public abstract int Align { get; }
}

/// <summary>A fixed-width integer: u8, i8, u16, i16, u32, i32, u64, i64.</summary>
public sealed record XfilInt(string Name) : XfilType
{
    public static readonly IReadOnlyList<string> Names = new[] { "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64" };

    public override int Size => Name[1..] switch { "8" => 1, "16" => 2, "32" => 4, _ => 8 };
    public override int Align => Size;
    public bool Signed => Name[0] == 'i';

    public string CType => (Signed ? "int" : "uint") + Name[1..] + "_t";
}

/// <summary>
/// <c>char[N]</c> (text, C <c>char</c>, X <c>u8</c>) or <c>T[N]</c> for an integer
/// <c>T</c>. <see cref="LengthText"/> is how the length was written: a literal or
/// a constant's name, kept so the C header says <c>FILE_NAME_MAX</c>, not 64.
/// </summary>
public sealed record XfilArray(XfilInt? Element, int Length, string LengthText) : XfilType
{
    public bool IsText => Element is null;
    public override int Size => (Element?.Size ?? 1) * Length;
    public override int Align => Element?.Align ?? 1;
}

/// <summary>A pointer parameter of a syscall wrapper: <c>*u8</c> or <c>*struct_name</c>. Never a structure field (§5: user addresses in structures are integers).</summary>
public sealed record XfilPointer(string Pointee) : XfilType
{
    // A pointer is never laid out: the parser refuses one as a field.
    public override int Size => 0;
    public override int Align => 0;
}

public abstract record XfilItem(XfilDoc Doc, int Line);

/// <summary>A <c>///</c> block standing on its own: a section heading or a note.</summary>
public sealed record XfilNote(XfilDoc Doc, int Line) : XfilItem(Doc, Line);

/// <summary>
/// <c>const NAME: T = value</c>. <see cref="ValueText"/> is the value as written
/// (a literal with its <c>u</c> suffix, or an OR of earlier constants), so the C
/// header keeps the exact tokens and the kernel compiles to the same bytes.
/// </summary>
public sealed record XfilConst(string Name, XfilInt Type, long Value, string ValueText, XfilDoc Doc, int Line) : XfilItem(Doc, Line);

public sealed record XfilEnumMember(string Name, long Value, string ValueText, XfilDoc Doc, int Line);

/// <summary><c>enum name { A = 0 ... }</c>: u32 constants that belong together. Every value is written out.</summary>
public sealed record XfilEnum(string Name, IReadOnlyList<XfilEnumMember> Members, XfilDoc Doc, int Line) : XfilItem(Doc, Line);

public sealed record XfilField(string Name, XfilType Type, int Offset, XfilDoc Doc, int Line);

/// <summary>A structure crossing the boundary. Fields in order, offsets computed, no implicit padding (§5).</summary>
public sealed record XfilStruct(string Name, IReadOnlyList<XfilField> Fields, int Size, XfilDoc Doc, int Line) : XfilItem(Doc, Line);

public sealed record XfilParam(string Name, XfilType Type);

/// <summary>The X wrapper a syscall is called through: <c>fn name(params) -> ret</c>. <see cref="Returns"/> is <c>i32</c> or <c>void</c>.</summary>
public sealed record XfilWrapper(string Name, IReadOnlyList<XfilParam> Params, string Returns);

/// <summary>
/// <c>syscall SYS_NAME = number [fn wrapper](params) -> ret</c>. The number is a
/// u32 constant named <see cref="Name"/>. Every syscall declares its arguments,
/// because the kernel's dispatch table decodes them (<see cref="KernelHandler"/>);
/// only one with a <see cref="WrapperName"/> gets a plain X wrapper.
/// </summary>
public sealed record XfilSyscall(string Name, long Number, string NumberText, IReadOnlyList<XfilParam> Params, string Returns,
                                 string? WrapperName, XfilDoc Doc, int Line) : XfilItem(Doc, Line)
{
    public XfilWrapper? Wrapper => WrapperName is null ? null : new XfilWrapper(WrapperName, Params, Returns);

    /// <summary>The kernel function the dispatch table calls: <c>SYS_FILE_READ</c>'s is <c>syscall_file_read</c>.</summary>
    public string KernelHandler => "syscall_" + Name[4..].ToLowerInvariant();
}

/// <summary>One parsed <c>.xfil</c> file: its header lines, then its items in file order.</summary>
public sealed record XfilFile(int Version, string Library, string Domain, IReadOnlyList<XfilItem> Items)
{
    public IEnumerable<XfilSyscall> Syscalls => Items.OfType<XfilSyscall>();
    public IEnumerable<XfilStruct> Structs => Items.OfType<XfilStruct>();
}

/// <summary>A <c>.xfil</c> file that does not parse or check, with the line it failed on.</summary>
public sealed class XfilException(int line, string message) : Exception($"line {line}: {message}")
{
    public int Line { get; } = line;
}
