// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;

namespace CXEX.Lang.Abi;

/// <summary>One ABI constant: a <c>const</c>, enum member or syscall number.</summary>
public sealed record AbiConstant(string Name, long Value, string? DeclaredXType = null)
{
    /// <summary>
    /// The X type it is declared with: the <c>.xfil</c>'s, or for one read from a
    /// C header, signed for error codes (negative) and unsigned for everything
    /// else (numbers, flags, op selectors, sizes).
    /// </summary>
    public string XType => DeclaredXType ?? (Name.StartsWith("E_", StringComparison.Ordinal) || Value < 0 ? "i32" : "u32");
}

/// <summary>One field of an ABI structure, with its C type and the X type it maps to.</summary>
public sealed record AbiField(string Name, string CType, string XType);

/// <summary>One ABI structure, fields in declaration order.</summary>
public sealed record AbiStruct(string Name, IReadOnlyList<AbiField> Fields);

/// <summary>
/// The CXK ABI as data (DevKit engineering review §7). Everything X sees of the
/// ABI is generated from this: <see cref="AbiPrelude"/> emits its constants and
/// structures from <see cref="EmitX"/>.
///
/// <para>Since roadmap step 6a1 the model comes from <c>abi/kernel.xfil</c>
/// (<see cref="XfilEmit.Model"/>), which is embedded into this assembly at build
/// time because a released <c>cxk</c> has no repository beside it. That makes the
/// build the moment the two are tied together: change the interface, rebuild the
/// DevKit, and the prelude follows with nobody editing it.</para>
///
/// <para><see cref="Parse"/> still reads a C header: the generated
/// <c>abi/cxk_abi.h</c> is read back with it, by <c>check-abi</c> and the tests, so
/// what C compiles and what X is given are held to each other. The parser is
/// deliberately strict. It understands exactly what the
/// header uses - integer <c>#define</c>s, OR-expressions of earlier constants,
/// anonymous <c>enum</c>s, and structures of fixed-width integers, pointers and
/// arrays - and throws on anything else. A generator that skipped what it did
/// not understand would quietly drop ABI surface, which is the failure this
/// exists to end.</para>
/// </summary>
public sealed class AbiModel
{
    public IReadOnlyList<AbiConstant> Constants { get; }
    public IReadOnlyList<AbiStruct> Structs { get; }

    private readonly Dictionary<string, AbiConstant> _byName;

    private AbiModel(List<AbiConstant> constants, List<AbiStruct> structs)
    {
        Constants = constants;
        Structs = structs;
        _byName = constants.ToDictionary(c => c.Name, StringComparer.Ordinal);
    }

    public AbiConstant? Constant(string name) => _byName.GetValueOrDefault(name);

    /// <summary>A model from parts already read (<see cref="XfilEmit.Model"/>).</summary>
    public static AbiModel Create(IEnumerable<AbiConstant> constants, IEnumerable<AbiStruct> structs)
    {
        var cs = constants.ToList();
        var dup = cs.GroupBy(c => c.Name, StringComparer.Ordinal).FirstOrDefault(g => g.Count() > 1);
        if (dup is not null) throw new InvalidDataException($"{dup.Key} is defined twice.");
        return new AbiModel(cs, structs.ToList());
    }

    /// <summary>The kernel interface (<c>abi/kernel.xfil</c>) this assembly was built from.</summary>
    public static string EmbeddedXfil => _embedded.Value;

    /// <summary><see cref="EmbeddedXfil"/>, parsed.</summary>
    public static XfilFile EmbeddedFile => _embeddedFile.Value;

    /// <summary>The header <see cref="EmbeddedXfil"/> generates.</summary>
    public static string GeneratedHeader => XfilEmit.CHeader(EmbeddedFile);

    /// <summary>The model of <see cref="EmbeddedXfil"/>.</summary>
    public static AbiModel Embedded => _embeddedModel.Value;

    private static readonly Lazy<string> _embedded = new(() =>
    {
        using Stream s = typeof(AbiModel).Assembly.GetManifestResourceStream("kernel.xfil")
            ?? throw new InvalidOperationException(
                "kernel.xfil is not embedded in CXEX.Lang; the DevKit was built without abi/kernel.xfil.");
        using var r = new StreamReader(s, Encoding.UTF8);
        return r.ReadToEnd();
    });

    private static readonly Lazy<XfilFile> _embeddedFile = new(() => XfilParser.Parse(EmbeddedXfil));
    private static readonly Lazy<AbiModel> _embeddedModel = new(() => XfilEmit.Model(EmbeddedFile));

    /// <summary>Constant prefixes of the header that are not ABI surface.</summary>
    private static readonly HashSet<string> NotSurface = new(StringComparer.Ordinal)
    {
        "CXK_ABI_H",   // include guard
    };

    private static readonly Regex Declaration = new(
        @"^[ \t]*#define[ \t]+(?<dname>[A-Za-z_]\w*)(?<drest>[^\n]*)$" +
        @"|enum\s*(?:[A-Za-z_]\w*\s*)?\{(?<enum>[^}]*)\}" +
        @"|struct\s+(?<sname>[A-Za-z_]\w*)\s*\{(?<sbody>[^}]*)\}",
        RegexOptions.Multiline | RegexOptions.CultureInvariant);

    /// <summary>Reads a header. Throws <see cref="InvalidDataException"/> on anything it cannot represent.</summary>
    public static AbiModel Parse(string headerText)
    {
        string text = AbiSync.StripComments(headerText.Replace("\r\n", "\n"));
        var constants = new List<AbiConstant>();
        var known = new Dictionary<string, long>(StringComparer.Ordinal);
        var structs = new List<AbiStruct>();

        void Add(string name, long value)
        {
            if (known.TryGetValue(name, out long prior))
            {
                if (prior != value)
                    throw new InvalidDataException($"{name} is defined twice, as {prior} and {value}.");
                return;
            }
            known[name] = value;
            constants.Add(new AbiConstant(name, value));
        }

        foreach (Match m in Declaration.Matches(text))
        {
            if (m.Groups["dname"].Success)
            {
                string name = m.Groups["dname"].Value;
                string rest = m.Groups["drest"].Value;
                if (rest.StartsWith('(')) continue;                  // function-like macro
                if (rest.Trim().Length == 0 || NotSurface.Contains(name)) continue;   // guard
                Add(name, Evaluate(name, rest, known));
            }
            else if (m.Groups["enum"].Success)
            {
                long next = 0;
                foreach (string piece in m.Groups["enum"].Value.Split(','))
                {
                    string p = piece.Trim();
                    if (p.Length == 0) continue;
                    int eq = p.IndexOf('=');
                    string name = (eq > 0 ? p[..eq] : p).Trim();
                    long value = eq > 0 ? Evaluate(name, p[(eq + 1)..], known) : next;
                    if (!Regex.IsMatch(name, @"^[A-Za-z_]\w*$"))
                        throw new InvalidDataException($"enum member '{p}' is not a plain name.");
                    Add(name, value);
                    next = value + 1;
                }
            }
            else
            {
                structs.Add(ParseStruct(m.Groups["sname"].Value, m.Groups["sbody"].Value, known));
            }
        }

        return new AbiModel(constants, structs);
    }

    /// <summary>A number, or an OR of numbers and earlier constants: all the header uses.</summary>
    private static long Evaluate(string name, string expression, Dictionary<string, long> known)
    {
        string e = expression.Trim();
        while (e.Length > 1 && e[0] == '(' && e[^1] == ')') e = e[1..^1].Trim();

        long result = 0;
        foreach (string term in e.Split('|'))
        {
            string t = term.Trim();
            if (AbiSync.TryParseNumber(t, out long v)) result |= v;
            else if (known.TryGetValue(t, out long k)) result |= k;
            else
                throw new InvalidDataException(
                    $"{name}: cannot evaluate '{expression.Trim()}'. The generator understands integers and " +
                    "ORs of earlier constants; extend it rather than let it drop ABI surface.");
        }
        return result;
    }

    private static readonly Regex Declarator = new(
        @"^(?<stars>[\s\*]*)(?<name>[A-Za-z_]\w*)\s*(?:\[(?<arr>[^\]]+)\])?\s*$",
        RegexOptions.CultureInvariant);

    private static AbiStruct ParseStruct(string name, string body, Dictionary<string, long> known)
    {
        var fields = new List<AbiField>();
        foreach (string rawDecl in body.Split(';'))
        {
            string decl = rawDecl.Trim();
            if (decl.Length == 0) continue;

            // The base type is everything before the first declarator. A later
            // declarator in `int32_t x, y` reuses it.
            string[] parts = decl.Split(',');
            string first = parts[0].Trim();
            string firstName = AbiSync.LastIdentifier(first);
            int at = first.LastIndexOf(firstName, StringComparison.Ordinal);
            string baseType = first[..at].Replace("*", " ").Trim();
            if (baseType.Length == 0)
                throw new InvalidDataException($"struct {name}: cannot read the type of '{decl}'.");

            for (int i = 0; i < parts.Length; i++)
            {
                string declarator = i == 0 ? first[at..] : parts[i];
                int stars = i == 0 ? first[..at].Count(c => c == '*') : 0;
                var d = Declarator.Match(declarator.Trim());
                if (!d.Success)
                    throw new InvalidDataException($"struct {name}: cannot read the declarator '{declarator.Trim()}'.");
                stars += d.Groups["stars"].Value.Count(c => c == '*');

                string cType = baseType + new string('*', stars) +
                               (d.Groups["arr"].Success ? $"[{d.Groups["arr"].Value.Trim()}]" : "");
                string xType = XTypeOf(name, baseType, stars, d.Groups["arr"], known);
                fields.Add(new AbiField(d.Groups["name"].Value, cType, xType));
            }
        }
        return new AbiStruct(name, fields);
    }

    private static string XTypeOf(string structName, string cBase, int stars, Group array, Dictionary<string, long> known)
    {
        string b = Regex.Replace(cBase, @"\b(const|volatile|struct)\b", " ").Trim();
        b = Regex.Replace(b, @"\s+", " ");

        string? x = b switch
        {
            "uint8_t" or "char" or "unsigned char" => "u8",
            "int8_t" or "signed char" => "i8",
            "uint16_t" => "u16",
            "int16_t" => "i16",
            "uint32_t" => "u32",
            "int32_t" or "int" => "i32",
            "uint64_t" => "u64",
            "int64_t" => "i64",
            "void" when stars > 0 => "u8",   // an untyped buffer is bytes in X
            _ => null,
        };
        if (x is null)
            throw new InvalidDataException($"struct {structName}: C type '{cBase}' has no X mapping.");

        string t = new string('*', stars) + x;
        if (array.Success)
        {
            long n = Evaluate($"struct {structName} array bound", array.Value, known);
            t = $"[{n}]{t}";
        }
        return t;
    }

    /// <summary>
    /// The X declarations: every constant, then every structure, in header
    /// order. Deterministic - the same header always yields the same text
    /// (CX_EXTENSION_SYSTEM.md §9.9, which counts the prelude as compiler input).
    /// </summary>
    public string EmitX()
    {
        var sb = new StringBuilder();
        sb.Append("// ---- generated from abi/kernel.xfil: constants ----\n");
        int width = Constants.Max(c => c.Name.Length + c.XType.Length + 2);
        foreach (var c in Constants)
        {
            string decl = $"{c.Name}: {c.XType}";
            string value = c.Value < 0 || c.Value < 10 ? c.Value.ToString(System.Globalization.CultureInfo.InvariantCulture)
                                                        : $"0x{c.Value:X}";
            sb.Append("const ").Append(decl.PadRight(width)).Append(" = ").Append(value).Append(";\n");
        }

        sb.Append("\n// ---- generated from abi/kernel.xfil: structures ----\n");
        foreach (var s in Structs)
            sb.Append("struct ").Append(s.Name).Append(" { ")
              .Append(string.Join(", ", s.Fields.Select(f => $"{f.Name}: {f.XType}")))
              .Append(" }\n");
        return sb.ToString();
    }
}
