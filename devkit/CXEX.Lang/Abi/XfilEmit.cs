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
    /// <summary>The kernel's syscall handlers and dispatch table (roadmap step 6a2), as repository paths.</summary>
    public const string KernelTableHeader = "kernel/cpu/systable.h";
    public const string KernelTableSource = "kernel/cpu/systable.c";

    private const int NameColumn = 24;

    /// <summary>Every file the interface generates, by repository path: what <c>cxk abi generate</c> writes and <c>check-abi</c> compares.</summary>
    public static IReadOnlyList<(string Path, string Text)> Outputs(XfilFile file) => new[]
    {
        (KernelHeader, CHeader(file)),
        (KernelTableHeader, CTableHeader(file)),
        (KernelTableSource, CTable(file)),
    };

    /// <summary>
    /// The kernel side of each syscall: one handler prototype per syscall, in file
    /// order, so the compiler holds every handler to the signature the interface
    /// declares, and the table's size.
    /// </summary>
    public static string CTableHeader(XfilFile file)
    {
        var sb = new StringBuilder();
        Banner(sb, KernelTableHeader, file.Library);
        sb.Append("""
#ifndef CXK_SYSTABLE_H
#define CXK_SYSTABLE_H

#include <stdint.h>

/* The handlers, one per syscall, named after it (SYS_FILE_READ: syscall_file_read).
 * Each is given its arguments as the interface declares them. A pointer arrives
 * as the user address it is, a uint32_t; the handler checks and copies it. A
 * u64 or i64 arrives whole: x86-32 carries it in two registers, low then high,
 * and its decoder joins them; x86-64 carries it in one. */


""");
        foreach (XfilSyscall s in file.Syscalls)
        {
            string ps = s.Params.Count == 0 ? "void" : string.Join(", ", s.Params.Select(p => $"{CParam(p.Type)} {p.Name}"));
            Line(sb, $"{(s.Returns == "void" ? "void" : "int")} {s.KernelHandler}({ps});", s.Name);
        }

        long size = file.Syscalls.Select(s => s.Number).DefaultIfEmpty(-1).Max() + 1;
        sb.Append($"""

/* Registers a syscall carries after its number, and the table's slots: one per
 * number up to the largest. syscall_dispatch refuses a number past the end
 * before looking it up, and one that lands in a hole finds no decoder. */
#define SYSCALL_ARGS            {XfilParser.MaxSyscallArgs}
#define SYSCALL_TABLE_SIZE      {size}
#define SYSCALL_COUNT           {file.Syscalls.Count()}

/* An argument register is as wide as the machine's: 32 bits on x86-32, where a
 * u64 takes a register pair, and 64 on x86-64, where it takes one. */
#if UINTPTR_MAX == 0xFFFFFFFFu
#define SYSCALL_PAIRS           1
#else
#define SYSCALL_PAIRS           0
#endif

/* A decoder takes the raw argument registers, gives each its declared type,
 * and calls the handler; a handler that returns nothing answers 0. */
typedef int (*syscall_decoder)(const uintptr_t *args);
extern const syscall_decoder syscall_table[SYSCALL_TABLE_SIZE];

#endif

""");
        return sb.ToString();
    }

    /// <summary>The decoders and the dispatch table, indexed by syscall number; holes are null.</summary>
    public static string CTable(XfilFile file)
    {
        var sb = new StringBuilder();
        Banner(sb, KernelTableSource, file.Library);
        sb.Append("#include \"systable.h\"\n#include \"cxk_abi.h\"   /* the SYS_* numbers */\n\n");
        foreach (XfilSyscall s in file.Syscalls)
        {
            string head = $"static int decode_{s.KernelHandler[8..]}(const uintptr_t *a)";
            if (!s.Params.Any(p => p.Wide))
            {
                sb.Append($"{head} {{ {DecodeBody(s, pairs: false)} }}\n");
                continue;
            }
            // The registers a u64 sits in differ by machine, and so does every
            // index after it.
            sb.Append($"{head} {{\n#if SYSCALL_PAIRS\n    {DecodeBody(s, pairs: true)}\n#else\n    {DecodeBody(s, pairs: false)}\n#endif\n}}\n");
        }
        sb.Append("\nconst syscall_decoder syscall_table[SYSCALL_TABLE_SIZE] = {\n");
        foreach (XfilSyscall s in file.Syscalls.OrderBy(s => s.Number))
            sb.Append($"    [{s.Name}] = decode_{s.KernelHandler[8..]},\n");
        sb.Append("};\n");
        return sb.ToString();
    }

    /// <summary>
    /// A decoder's statement: each argument register given its declared type. With
    /// <paramref name="pairs"/> a u64 or i64 is joined from two registers, low
    /// first; without, it is one.
    /// </summary>
    private static string DecodeBody(XfilSyscall s, bool pairs)
    {
        var args = new List<string>();
        int r = 0;
        foreach (XfilParam p in s.Params)
        {
            string type = p.Type is XfilInt i ? i.CType : "uint32_t";
            if (p.Wide && pairs)
            {
                args.Add($"({type})((uint64_t)a[{r}] | (uint64_t)a[{r + 1}] << 32)");
                r += 2;
            }
            else
                args.Add($"({type})a[{r++}]");
        }
        string call = $"{s.KernelHandler}({string.Join(", ", args)})";
        string unused = s.Params.Count == 0 ? "(void)a; " : "";
        return s.Returns == "void" ? $"{unused}{call}; return 0;" : $"{unused}return {call};";
    }

    private static string CParam(XfilType t) => t switch
    {
        XfilInt i => i.CType,
        XfilPointer => "uint32_t",
        _ => throw new InvalidOperationException($"no C parameter type for {t}"),
    };

    private static void Banner(StringBuilder sb, string path, string library)
    {
        sb.Append("// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0\n");
        sb.Append("// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)\n");
        sb.Append($"/* {path}: GENERATED from {KernelSource} ({library}) by `cxk abi generate`.\n");
        sb.Append($" * Do not edit it: change {KernelSource} and regenerate. `cxk check-abi` fails while\n");
        sb.Append(" * this file differs from what the .xfil generates. */\n\n");
    }

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
        if (file.Hash.Length > 0)
        {
            sb.Append("\n/*\n * The ABI hash: SHA-256 of ").Append(KernelSource).Append(" without its comments\n");
            sb.Append(" * (docs/kernel/CX_SYSCALLS_IPC.md sec 5, 9.5). Every CXEX image records the\n");
            sb.Append(" * hash it was built against and the loader refuses one that differs, so a\n");
            sb.Append(" * program never runs against numbers or layouts it was not compiled for.\n */\n");
            sb.Append("#define CXK_ABI_HASH { ")
              .Append(string.Join(", ", file.Hash.Select(b => $"0x{b:x2}")))
              .Append(" }\n");
        }

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
    /// number, then each argument as a register, padded to the five
    /// <c>__syscall</c> takes. With <paramref name="pairs"/> (x86-32) a register
    /// is a u32: a u32 goes as it is, anything else is cast, and a u64 or i64
    /// takes two, low then high. Without (x86-64, roadmap step 6d1a) a register
    /// is 64 bits: a u64, an i64 and a pointer go whole, as the decoder's
    /// <c>SYSCALL_PAIRS</c> 0 reads them.
    /// </summary>
    public static string XWrappers(XfilFile file, bool pairs = true)
    {
        var sb = new StringBuilder("// ---- typed syscall wrappers ----\n");
        foreach (XfilSyscall s in file.Syscalls)
        {
            if (s.Wrapper is not { } w) continue;
            var args = new List<string> { s.Name };
            foreach (XfilParam p in w.Params)
            {
                if (p.Wide && pairs)
                {
                    args.Add($"{p.Name} as u32");
                    args.Add($"({p.Name} >> 32) as u32");
                }
                else if (p.Wide || (!pairs && p.Type is XfilPointer))
                    args.Add(p.Name);
                else
                    args.Add(p.Type is XfilInt { Name: "u32" } ? p.Name : $"{p.Name} as u32");
            }
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
