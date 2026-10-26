// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Globalization;
using System.Text.RegularExpressions;

namespace CXEX.Lang.Abi;

/// <summary>
/// Reads a <c>.xfil</c> file (X Format Interface Language,
/// <c>docs/kernel/CX_SYSCALLS_IPC.md</c> §9) into an <see cref="XfilFile"/>.
///
/// <para>The language is line-oriented: one declaration, member or field per
/// line. <c>//</c> starts a comment that stays in the file; <c>///</c> starts
/// documentation, which the generators carry into what they write. Documentation
/// lines directly above an item belong to it; a block followed by a blank line
/// stands on its own (a section heading). A <c>///</c> after a declaration on the
/// same line is that declaration's trailing note.</para>
///
/// <para>Strict, as <see cref="AbiModel"/> was before it: anything it does not
/// understand is an <see cref="XfilException"/> naming the line, never skipped.
/// A generator that dropped what it could not read would drop ABI surface.</para>
///
/// <para>The checks that make an interface safe to generate from are here, not in
/// the emitters: names unique, values in range for their type, array lengths
/// known, structures with no implicit padding (§5), wrappers whose pointers name
/// a declared structure, and no documentation text that would end a C comment.</para>
/// </summary>
public static class XfilParser
{
    /// <summary>The <c>.xfil</c> versions this parser reads.</summary>
    public const int Version = 1;

    /// <summary>Domains a <c>.xfil</c> file may declare (§9.1).</summary>
    public static readonly IReadOnlyList<string> Domains = new[] { "kernel", "boot", "os", "system", "user" };

    /// <summary>A syscall carries at most this many arguments (eax holds the number; ebx..edi the rest).</summary>
    public const int MaxSyscallArgs = 5;

    /// <summary>The largest syscall number: the kernel's dispatch table has a slot for each one up to the largest used.</summary>
    public const long MaxSyscallNumber = 0x3FF;

    private static readonly Regex Name = new(@"^[A-Za-z_][A-Za-z0-9_]*$", RegexOptions.CultureInvariant);
    private static readonly Regex Literal = new(@"^-?(0[xX][0-9A-Fa-f]+|[0-9]+)[uU]?$", RegexOptions.CultureInvariant);
    private static readonly Regex LibraryName = new(@"^[a-z][a-z0-9]*(\.[a-z][a-z0-9_]*)+$", RegexOptions.CultureInvariant);

    private static readonly Regex ConstDecl = new(@"^const\s+(?<name>\S+)\s*:\s*(?<type>\S+)\s*=\s*(?<value>.+)$", RegexOptions.CultureInvariant);
    private static readonly Regex OpenDecl = new(@"^(?<kw>enum|struct)\s+(?<name>\S+)\s*\{$", RegexOptions.CultureInvariant);
    private static readonly Regex SyscallDecl = new(
        @"^syscall\s+(?<name>\S+)\s*=\s*(?<num>[^\s(]+)\s*(?:fn\s+(?<fn>[A-Za-z_]\w*)\s*)?\((?<params>[^)]*)\)\s*->\s*(?<ret>\S+)$",
        RegexOptions.CultureInvariant);
    private static readonly Regex MemberDecl = new(@"^(?<name>\S+)\s*=\s*(?<value>.+)$", RegexOptions.CultureInvariant);
    private static readonly Regex FieldDecl = new(@"^(?<name>\S+)\s*:\s*(?<type>.+)$", RegexOptions.CultureInvariant);
    private static readonly Regex ArrayType = new(@"^(?<elem>\w+)\[(?<len>[^\]]+)\]$", RegexOptions.CultureInvariant);

    public static XfilFile Parse(string text) => new Reader(text).Read();

    private sealed class Reader
    {
        private readonly string[] _lines;
        private int _at;                       // index of the next line to read
        private readonly List<string> _doc = new();
        private int _docLine;

        private readonly List<XfilItem> _items = new();
        private readonly Dictionary<string, XfilInt> _constTypes = new(StringComparer.Ordinal);
        private readonly Dictionary<string, long> _values = new(StringComparer.Ordinal);
        private readonly HashSet<string> _structs = new(StringComparer.Ordinal);
        private readonly HashSet<string> _wrappers = new(StringComparer.Ordinal);
        private readonly Dictionary<long, string> _numbers = new();

        public Reader(string text) => _lines = text.Replace("\r\n", "\n").Split('\n');

        private int LineNo => _at;   // 1-based number of the line just read

        public XfilFile Read()
        {
            int version = int.Parse(Header("xfil", v => v == "1"
                ? null : $"this parser reads xfil {Version}; the file says '{v}'"), CultureInfo.InvariantCulture);
            string library = Header("library", v => LibraryName.IsMatch(v)
                ? null : $"'{v}' is not a reverse-dotted library name (lower case, at least two parts)");
            string domain = Header("domain", v => Domains.Contains(v)
                ? null : $"'{v}' is not a domain; one of {string.Join(", ", Domains)}");
            if (domain == "kernel" && library != "cx.kernel")
                throw new XfilException(LineNo, $"the kernel domain has exactly one library, cx.kernel, not '{library}'");

            while (Next(out string line, out string? trailing))
            {
                if (line.Length == 0) { FlushNote(); continue; }
                XfilItem item = Declaration(line, trailing);
                _items.Add(item);
            }
            FlushNote();

            foreach (var s in _items.OfType<XfilSyscall>())
                foreach (var p in s.Params)
                    if (p.Type is XfilPointer { Pointee: var t } && t != "u8" && !_structs.Contains(t))
                        throw new XfilException(s.Line, $"{s.WrapperName ?? s.Name}: parameter {p.Name} points at '{t}', which is not a structure in this file");

            return new XfilFile(version, library, domain, _items);
        }

        /// <summary>Reads one of the three header lines: comments may come before it, nothing else.</summary>
        private string Header(string keyword, Func<string, string?> check)
        {
            if (!Next(out string line, out string? trailing, headerPhase: true))
                throw new XfilException(LineNo, $"the file ends before its '{keyword}' line");
            if (_doc.Count > 0 || trailing is not null)
                throw new XfilException(LineNo, "documentation (///) cannot come before or on the header lines");
            string[] parts = line.Split(' ', 2, StringSplitOptions.TrimEntries);
            if (parts.Length != 2 || parts[0] != keyword)
                throw new XfilException(LineNo, $"expected '{keyword} ...' (every .xfil starts: xfil, library, domain); found '{line}'");
            string? problem = check(parts[1]);
            if (problem is not null) throw new XfilException(LineNo, problem);
            return parts[1];
        }

        /// <summary>
        /// The next line that carries something: its code, trimmed, with any
        /// trailing <c>///</c> note split off. Whole-line <c>///</c> lines are
        /// collected as pending documentation and <c>//</c> lines skipped. Blank
        /// lines are returned as "" (they end a documentation block), except while
        /// reading the header, where they are skipped.
        /// </summary>
        private bool Next(out string code, out string? trailing, bool headerPhase = false)
        {
            while (_at < _lines.Length)
            {
                string raw = _lines[_at++];
                string t = raw.Trim();
                if (t.StartsWith("///", StringComparison.Ordinal))
                {
                    if (_doc.Count == 0) _docLine = LineNo;
                    _doc.Add(DocText(t[3..]));
                    continue;
                }
                if (t.StartsWith("//", StringComparison.Ordinal)) continue;
                if (t.Length == 0 && headerPhase) continue;

                trailing = null;
                int d = t.IndexOf("///", StringComparison.Ordinal);
                if (d >= 0)
                {
                    trailing = DocText(t[(d + 3)..]).Trim();
                    t = t[..d].TrimEnd();
                }
                int c = t.IndexOf("//", StringComparison.Ordinal);
                if (c >= 0) t = t[..c].TrimEnd();
                code = t;
                return true;
            }
            code = "";
            trailing = null;
            return false;
        }

        private string DocText(string afterSlashes)
        {
            string s = afterSlashes.StartsWith(' ') ? afterSlashes[1..] : afterSlashes;
            s = s.TrimEnd();
            if (s.Contains("*/", StringComparison.Ordinal) || s.Contains("/*", StringComparison.Ordinal))
                throw new XfilException(LineNo, "documentation may not contain '/*' or '*/': it is carried into C comments");
            return s;
        }

        private XfilDoc TakeDoc(string? trailing)
        {
            var doc = new XfilDoc(_doc.ToArray(), string.IsNullOrEmpty(trailing) ? null : trailing);
            _doc.Clear();
            return doc;
        }

        private void FlushNote()
        {
            if (_doc.Count == 0) return;
            _items.Add(new XfilNote(TakeDoc(null), _docLine));
        }

        private XfilItem Declaration(string line, string? trailing)
        {
            int at = LineNo;
            Match m;
            if ((m = ConstDecl.Match(line)).Success)
            {
                string name = NewConstant(m.Groups["name"].Value);
                XfilInt type = ConstantType(m.Groups["type"].Value);
                (long v, string text) = Value(name, m.Groups["value"].Value, type);
                Define(name, type, v);
                return new XfilConst(name, type, v, text, TakeDoc(trailing), at);
            }
            if ((m = SyscallDecl.Match(line)).Success)
            {
                string name = NewConstant(m.Groups["name"].Value);
                var u32 = new XfilInt("u32");
                (long number, string text) = Value(name, m.Groups["num"].Value, u32);
                Define(name, u32, number);
                if (!name.StartsWith("SYS_", StringComparison.Ordinal) || name.Length == 4)
                    throw new XfilException(at, $"{name}: a syscall's name starts with SYS_, and its kernel handler is named after the rest");
                if (number > MaxSyscallNumber)
                    throw new XfilException(at, $"{name} is number {number}; the dispatch table has a slot per number, up to {MaxSyscallNumber}");
                if (_numbers.TryGetValue(number, out string? taken))
                    throw new XfilException(at, $"{name} has number {number}, which is {taken}'s");
                _numbers[number] = name;
                string? fn = m.Groups["fn"].Success ? m.Groups["fn"].Value : null;
                if (fn is not null && !_wrappers.Add(fn)) throw new XfilException(at, $"wrapper {fn} is declared twice");
                (var ps, string ret) = Signature(fn ?? name, m.Groups["params"].Value, m.Groups["ret"].Value);
                return new XfilSyscall(name, number, text, ps, ret, fn, TakeDoc(trailing), at);
            }
            if ((m = OpenDecl.Match(line)).Success)
            {
                string name = m.Groups["name"].Value;
                if (!Name.IsMatch(name)) throw new XfilException(at, $"'{name}' is not a name");
                XfilDoc doc = TakeDoc(trailing);
                return m.Groups["kw"].Value == "enum" ? Enum(name, doc, at) : Struct(name, doc, at);
            }
            if (line.StartsWith("syscall", StringComparison.Ordinal))
                throw new XfilException(at, $"cannot read '{line}': syscall NAME = number [fn wrapper](params) -> i32|void; " +
                                            "every syscall declares its arguments, which the kernel's dispatch table decodes");
            throw new XfilException(at, $"cannot read '{line}': expected const, enum, struct or syscall");
        }

        private XfilEnum Enum(string name, XfilDoc doc, int at)
        {
            var members = new List<XfilEnumMember>();
            var u32 = new XfilInt("u32");
            foreach ((string line, string? trailing, int no) in Body(name))
            {
                Match m = MemberDecl.Match(line);
                if (!m.Success) throw new XfilException(no, $"enum {name}: expected 'NAME = value', found '{line}'");
                string member = NewConstant(m.Groups["name"].Value);
                (long v, string text) = Value(member, m.Groups["value"].Value, u32);
                Define(member, u32, v);
                members.Add(new XfilEnumMember(member, v, text, TakeDoc(trailing), no));
            }
            if (members.Count == 0) throw new XfilException(at, $"enum {name} has no members");
            return new XfilEnum(name, members, doc, at);
        }

        private XfilStruct Struct(string name, XfilDoc doc, int at)
        {
            if (!_structs.Add(name)) throw new XfilException(at, $"struct {name} is declared twice");
            var fields = new List<XfilField>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            int offset = 0, align = 1;
            foreach ((string line, string? trailing, int no) in Body(name))
            {
                Match m = FieldDecl.Match(line);
                if (!m.Success) throw new XfilException(no, $"struct {name}: expected 'name: type', found '{line}'");
                string field = m.Groups["name"].Value;
                if (!Name.IsMatch(field)) throw new XfilException(no, $"struct {name}: '{field}' is not a name");
                if (!seen.Add(field)) throw new XfilException(no, $"struct {name}: field {field} is declared twice");
                XfilType type = FieldType(name, m.Groups["type"].Value.Trim(), no);
                if (offset % type.Align != 0)
                    throw new XfilException(no,
                        $"struct {name}: {field} would sit at offset {offset}, which a {type.Align}-byte field cannot; " +
                        "the compiler would pad silently. Declare the padding as a field of its own (§5).");
                fields.Add(new XfilField(field, type, offset, TakeDoc(trailing), no));
                offset += type.Size;
                align = Math.Max(align, type.Align);
            }
            if (fields.Count == 0) throw new XfilException(at, $"struct {name} has no fields");
            if (offset % align != 0)
                throw new XfilException(at,
                    $"struct {name} is {offset} bytes, which its {align}-byte alignment would pad at the end. " +
                    "Declare the padding as a field of its own (§5).");
            return new XfilStruct(name, fields, offset, doc, at);
        }

        /// <summary>The member or field lines of an enum or struct, up to its closing brace.</summary>
        private IEnumerable<(string Line, string? Trailing, int No)> Body(string owner)
        {
            while (true)
            {
                if (!Next(out string line, out string? trailing))
                    throw new XfilException(LineNo, $"{owner}: the file ends before its closing '}}'");
                if (line.Length == 0) continue;
                if (line == "}")
                {
                    if (_doc.Count > 0 || trailing is not null)
                        throw new XfilException(LineNo, $"{owner}: documentation before '}}' documents nothing");
                    yield break;
                }
                yield return (line, trailing, LineNo);
            }
        }

        /// <summary>A syscall's arguments and result. <paramref name="name"/> is its wrapper's, or the syscall's when it has none.</summary>
        private (IReadOnlyList<XfilParam>, string) Signature(string name, string paramText, string ret)
        {
            if (ret is not ("i32" or "void"))
                throw new XfilException(LineNo, $"{name}: a syscall returns i32 or void, not '{ret}'");
            var ps = new List<XfilParam>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (string raw in paramText.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
            {
                string[] nt = raw.Split(':', 2, StringSplitOptions.TrimEntries);
                if (nt.Length != 2 || !Name.IsMatch(nt[0]))
                    throw new XfilException(LineNo, $"{name}: cannot read the parameter '{raw}' (name: type)");
                if (!seen.Add(nt[0])) throw new XfilException(LineNo, $"{name}: parameter {nt[0]} is declared twice");
                XfilType type = nt[1] switch
                {
                    "u32" or "i32" => new XfilInt(nt[1]),
                    var p when p.StartsWith('*') && Name.IsMatch(p[1..]) => new XfilPointer(p[1..]),
                    _ => throw new XfilException(LineNo,
                        $"{name}: parameter {nt[0]} is '{nt[1]}'; a register argument is u32, i32 or a pointer (*u8, *struct)"),
                };
                ps.Add(new XfilParam(nt[0], type));
            }
            if (ps.Count > MaxSyscallArgs)
                throw new XfilException(LineNo, $"{name} takes {ps.Count} arguments; a syscall carries at most {MaxSyscallArgs}");
            return (ps, ret);
        }

        private string NewConstant(string name)
        {
            if (!Name.IsMatch(name)) throw new XfilException(LineNo, $"'{name}' is not a name");
            if (_values.ContainsKey(name)) throw new XfilException(LineNo, $"{name} is declared twice");
            return name;
        }

        private void Define(string name, XfilInt type, long value)
        {
            _values[name] = value;
            _constTypes[name] = type;
        }

        private XfilInt ConstantType(string t) => t is "u32" or "i32"
            ? new XfilInt(t)
            : throw new XfilException(LineNo, $"a constant is u32 or i32, not '{t}'");

        private XfilType FieldType(string owner, string t, int no)
        {
            if (XfilInt.Names.Contains(t)) return new XfilInt(t);
            Match a = ArrayType.Match(t);
            if (a.Success)
            {
                string elem = a.Groups["elem"].Value;
                if (elem != "char" && !XfilInt.Names.Contains(elem))
                    throw new XfilException(no, $"struct {owner}: arrays are of char or a fixed-width integer, not '{elem}'");
                string lenText = a.Groups["len"].Value.Trim();
                long len;
                if (Literal.IsMatch(lenText)) len = ParseLiteral(lenText);
                else if (!_values.TryGetValue(lenText, out len))
                    throw new XfilException(no, $"struct {owner}: the array length '{lenText}' is not a number or an earlier constant");
                if (len < 1 || len > 65536)
                    throw new XfilException(no, $"struct {owner}: an array length must be 1..65536, not {len}");
                return new XfilArray(elem == "char" ? null : new XfilInt(elem), (int)len, lenText);
            }
            if (t.StartsWith('*'))
                throw new XfilException(no, $"struct {owner}: a structure holds no pointers; a user address is an integer field (§5)");
            throw new XfilException(no, $"struct {owner}: unknown type '{t}'");
        }

        /// <summary>
        /// A literal, or an OR of literals and earlier constants. Returns the value
        /// and its C text: a lone literal as written (its <c>u</c> suffix kept, a
        /// negative one parenthesised), an OR parenthesised.
        /// </summary>
        private (long Value, string CText) Value(string name, string expression, XfilInt type)
        {
            string e = expression.Trim();
            string[] terms = e.Split('|', StringSplitOptions.TrimEntries);
            long v = 0;
            foreach (string term in terms)
            {
                if (Literal.IsMatch(term))
                {
                    if (term.StartsWith('-') && terms.Length > 1)
                        throw new XfilException(LineNo, $"{name}: a negative number cannot be ORed");
                    v |= ParseLiteral(term);
                }
                else if (_values.TryGetValue(term, out long k)) v |= k;
                else
                    throw new XfilException(LineNo,
                        $"{name}: cannot evaluate '{term}'. A value is a number or an OR of earlier constants.");
            }
            (long lo, long hi) = type.Signed ? (int.MinValue, (long)int.MaxValue) : (0L, (long)uint.MaxValue);
            if (v < lo || v > hi) throw new XfilException(LineNo, $"{name}: {v} does not fit in {type.Name}");

            string text = terms.Length > 1 ? "(" + string.Join(" | ", terms) + ")"
                        : e.StartsWith('-') ? $"({e})"
                        : e;
            return (v, text);
        }

        private long ParseLiteral(string t)
        {
            bool negative = t.StartsWith('-');
            string s = (negative ? t[1..] : t).TrimEnd('u', 'U');
            long v;
            try
            {
                v = s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                    ? long.Parse(s[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
                    : long.Parse(s, NumberStyles.None, CultureInfo.InvariantCulture);
            }
            catch (OverflowException)
            {
                throw new XfilException(LineNo, $"'{t}' is too large");
            }
            if (v < 0) throw new XfilException(LineNo, $"'{t}' is too large");
            return negative ? -v : v;
        }
    }
}
