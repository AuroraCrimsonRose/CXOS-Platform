// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text;

namespace CXEX.Lang.Abi;

/// <summary>
/// What a <c>kernel</c>-domain <c>.xfil</c> generates (§9.3): the C header both
/// sides of the boundary compile against, and X's view of it. Deterministic: the
/// same file always yields the same text, because <c>check-abi</c> compares the
/// committed header with it byte for byte, and the prelude is compiler input
/// (<c>CX_EXTENSION_SYSTEM.md</c> §9.9).
/// </summary>
public static class XfilEmit
{
    /// <summary>The kernel interface's source and its generated header, as repository paths.</summary>
    public const string KernelSource = "abi/kernel.xfil";
    public const string KernelHeader = "abi/cxk_abi.h";

    private const int NameColumn = 24;

    /// <summary>The C header: constants as <c>#define</c>s with their tokens as written, structures, and a size assert for each.</summary>
    public static string CHeader(XfilFile file)
    {
        var sb = new StringBuilder();
        sb.Append("// SPDX-License-Identifier: MIT\n");
        sb.Append("// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)\n");
        sb.Append($"/* {KernelHeader}: GENERATED from {KernelSource} ({file.Library}) by `cxk abi generate`.\n");
        sb.Append($" * Do not edit it: change {KernelSource} and regenerate. `cxk check-abi` fails while\n");
        sb.Append(" * this file differs from what the .xfil generates. */\n\n");
        sb.Append("#ifndef CXK_ABI_H\n#define CXK_ABI_H\n\n#include <stdint.h>\n");

        foreach (XfilItem item in file.Items)
        {
            switch (item)
            {
                case XfilNote n:
                    sb.Append('\n');
                    Comment(sb, n.Doc.Lines, "");
                    break;
                case XfilConst c:
                    Define(sb, c.Name, c.ValueText, c.Doc);
                    break;
                case XfilSyscall s:
                    Define(sb, s.Name, s.NumberText, s.Doc);
                    break;
                case XfilEnum e:
                    sb.Append('\n');
                    Comment(sb, e.Doc.Lines, "");
                    sb.Append("enum { /* ").Append(e.Name).Append(" */\n");
                    foreach (var m in e.Members)
                    {
                        Comment(sb, m.Doc.Lines, "    ");
                        Line(sb, $"    {m.Name} = {m.ValueText},", m.Doc.Trailing);
                    }
                    sb.Append("};\n");
                    break;
                case XfilStruct st:
                    sb.Append('\n');
                    Comment(sb, st.Doc.Lines, "");
                    sb.Append("struct ").Append(st.Name).Append(" {\n");
                    foreach (var f in st.Fields)
                    {
                        Comment(sb, f.Doc.Lines, "    ");
                        Line(sb, $"    {CDeclaration(f)};", f.Doc.Trailing);
                    }
                    sb.Append("};\n");
                    break;
            }
        }

        sb.Append("""

/* ---- layout lock ----
 * Every structure above crosses the user/kernel boundary, so its size is the
 * contract, not an accident of the compiler. The generator refuses implicit
 * padding and computed these sizes; the compiler confirms them. No field is a
 * C pointer: a user address is an integer, so the kernel cannot dereference
 * one by accident - it goes through user_copy_in/out. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define CXK_ABI_STATIC_ASSERT(c, m) _Static_assert(c, m)
#else
  #define CXK_ABI_STATIC_ASSERT(c, m) typedef char cxk_abi_assert_##__LINE__[(c) ? 1 : -1]
#endif


""");
        foreach (XfilStruct st in file.Structs)
            sb.Append($"CXK_ABI_STATIC_ASSERT(sizeof(struct {st.Name}) == {st.Size}, \"ABI layout: {st.Name}\");\n");
        sb.Append("\n#endif\n");
        return sb.ToString();
    }

    /// <summary>
    /// X's typed wrapper for each syscall that declares one, in file order: the
    /// number, then each argument as a u32 register (a u32 as it is, anything
    /// else cast), padded to the five <c>__syscall</c> takes.
    /// </summary>
    public static string XWrappers(XfilFile file)
    {
        var sb = new StringBuilder("// ---- typed syscall wrappers ----\n");
        foreach (XfilSyscall s in file.Syscalls)
        {
            if (s.Wrapper is not { } w) continue;
            var args = new List<string> { s.Name };
            args.AddRange(w.Params.Select(p => p.Type is XfilInt { Name: "u32" } ? p.Name : $"{p.Name} as u32"));
            while (args.Count < XfilParser.MaxSyscallArgs + 1) args.Add("0");
            string ps = string.Join(", ", w.Params.Select(p => $"{p.Name}: {XType(p.Type)}"));
            string call = $"__syscall({string.Join(", ", args)});";
            sb.Append($"fn {w.Name}({ps}) -> {w.Returns} {{ {(w.Returns == "void" ? "" : "return ")}{call} }}\n");
        }
        return sb.Append('\n').ToString();
    }

    /// <summary>The model X's constants and structures are emitted from (<see cref="AbiModel.EmitX"/>): every constant in file order, then every structure.</summary>
    public static AbiModel Model(XfilFile file)
    {
        var constants = new List<AbiConstant>();
        var structs = new List<AbiStruct>();
        foreach (XfilItem item in file.Items)
        {
            switch (item)
            {
                case XfilConst c: constants.Add(new AbiConstant(c.Name, c.Value, c.Type.Name)); break;
                case XfilSyscall s: constants.Add(new AbiConstant(s.Name, s.Number, "u32")); break;
                case XfilEnum e: constants.AddRange(e.Members.Select(m => new AbiConstant(m.Name, m.Value, "u32"))); break;
                case XfilStruct st:
                    structs.Add(new AbiStruct(st.Name, st.Fields.Select(f => new AbiField(f.Name, CType(f.Type), XType(f.Type))).ToList()));
                    break;
            }
        }
        return AbiModel.Create(constants, structs);
    }

    public static string XType(XfilType t) => t switch
    {
        XfilInt i => i.Name,
        XfilArray a => $"[{a.Length}]{a.Element?.Name ?? "u8"}",
        XfilPointer p => "*" + p.Pointee,
        _ => throw new InvalidOperationException($"no X type for {t}"),
    };

    private static string CType(XfilType t) => t switch
    {
        XfilInt i => i.CType,
        XfilArray a => $"{(a.IsText ? "char" : a.Element!.CType)}[{a.LengthText}]",
        _ => throw new InvalidOperationException($"no C field type for {t}"),
    };

    private static string CDeclaration(XfilField f) => f.Type switch
    {
        XfilInt i => $"{i.CType} {f.Name}",
        XfilArray a => $"{(a.IsText ? "char" : a.Element!.CType)} {f.Name}[{a.LengthText}]",
        _ => throw new InvalidOperationException($"no C field type for {f.Type}"),
    };

    private static void Define(StringBuilder sb, string name, string value, XfilDoc doc)
    {
        if (doc.Lines.Count > 0)
        {
            sb.Append('\n');
            Comment(sb, doc.Lines, "");
        }
        Line(sb, $"#define {name.PadRight(NameColumn)} {value}", doc.Trailing);
    }

    private static void Line(StringBuilder sb, string code, string? trailing)
    {
        sb.Append(code);
        if (trailing is not null) sb.Append(code.Length < 48 ? new string(' ', 48 - code.Length) : " ").Append("/* ").Append(trailing).Append(" */");
        sb.Append('\n');
    }

    private static void Comment(StringBuilder sb, IReadOnlyList<string> lines, string indent)
    {
        if (lines.Count == 0) return;
        if (lines.Count == 1)
        {
            sb.Append(indent).Append("/* ").Append(lines[0]).Append(" */\n");
            return;
        }
        sb.Append(indent).Append("/*\n");
        foreach (string l in lines)
            sb.Append(indent).Append(l.Length == 0 ? " *" : " * " + l).Append('\n');
        sb.Append(indent).Append(" */\n");
    }
}
