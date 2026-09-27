using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace CXEX.Lang.Abi;

/// <summary>
/// Compares the kernel's ABI header (<c>CXK/abi/cxk_abi.h</c>) against the X prelude
/// <see cref="AbiPrelude"/> generates, and reports anything that has drifted.
///
/// <para><b>Why this exists.</b> The prelude emits a banner reading "GENERATED from
/// cxk_abi.h - Do not edit by hand", and nothing generates it: it is a hand-maintained
/// string literal living in a different repository from the header it claims to track.
/// That has already broken a build once. The kernel gained <c>SYS_MOUSE_READ</c> and
/// <c>struct mouse_state</c>, the prelude did not, and <c>gui.xfxn</c> referenced both, so
/// the committed compiler could not compile the committed OS. The drift was exactly one
/// syscall and one struct - small enough to be invisible, fatal enough to stop the
/// build.</para>
///
/// <para><b>The family rule.</b> The prelude deliberately mirrors only part of the header:
/// it carries <c>SYS_*</c>, <c>E_*</c>, <c>POWER_*</c> and <c>FB_OP_*</c>, but not
/// <c>CAP_*</c> or <c>NET_OP_*</c>. Demanding total parity would therefore report dozens of
/// false positives on day one and get ignored, which is worse than no check. So the rule is
/// <i>if you mirror a family, mirror all of it</i>: a family with at least one member in the
/// prelude must be complete, and a family with none is reported as information, not failure.
/// The day someone adds a single <c>NET_OP_</c> constant, the rest become required.</para>
///
/// <para>Struct fields are compared by name and order, because that is the memory layout
/// contract. A reordered struct still compiles on both sides and silently corrupts every
/// call that uses it, which is the worst failure mode available here.</para>
/// </summary>
public static class AbiSync
{
    /// <summary>Constant prefixes the prelude is expected to mirror, longest first so
    /// <c>NET_OP_</c> is not mistaken for a member of some shorter family.</summary>
    private static readonly string[] Families =
    {
        "FILE_OP_", "NET_OP_", "FOPEN_", "FSEEK_", "FTYPE_",
        "FB_OP_", "POWER_", "SYS_", "CAP_", "E_"
    };

    /// <summary>Header names that are not ABI surface and must not be reported.</summary>
    private static readonly HashSet<string> Ignored = new(StringComparer.Ordinal)
    {
        "CXK_ABI_H",           // include guard
    };

    public static AbiSyncReport Compare(string headerText, string preludeText)
    {
        var report = new AbiSyncReport();

        string header = StripComments(headerText);
        string prelude = StripComments(preludeText);

        Dictionary<string, long> hConst = ParseCDefines(header);
        AddCEnumMembers(header, hConst);
        Dictionary<string, long> pConst = ParseXConsts(prelude);

        Dictionary<string, List<string>> hStructs = ParseCStructs(header);
        Dictionary<string, List<string>> pStructs = ParseXStructs(prelude);

        report.HeaderConstants = hConst.Count;
        report.PreludeConstants = pConst.Count;
        report.HeaderStructs = hStructs.Count;
        report.PreludeStructs = pStructs.Count;

        // Which families does the prelude mirror at all?
        var mirrored = new HashSet<string>(StringComparer.Ordinal);
        foreach (string name in pConst.Keys)
        {
            string fam = FamilyOf(name);
            if (fam.Length > 0) mirrored.Add(fam);
        }

        // ---- constants ----
        var unmirrored = new SortedDictionary<string, List<string>>(StringComparer.Ordinal);
        var unfamilied = new List<string>();

        foreach (KeyValuePair<string, long> kv in Sorted(hConst))
        {
            if (Ignored.Contains(kv.Key)) continue;

            string fam = FamilyOf(kv.Key);
            if (fam.Length == 0)
            {
                if (!pConst.ContainsKey(kv.Key)) unfamilied.Add(kv.Key);
                continue;
            }

            if (!mirrored.Contains(fam))
            {
                if (!unmirrored.TryGetValue(fam, out List<string>? members))
                {
                    members = new List<string>();
                    unmirrored[fam] = members;
                }
                members.Add(kv.Key);
                continue;
            }

            if (!pConst.TryGetValue(kv.Key, out long pv))
            {
                report.Add(AbiFindingKind.Error,
                    $"{kv.Key} = {Hex(kv.Value)} is in the header but missing from the prelude " +
                    $"(the {fam}* family is mirrored, so it must be complete)");
            }
            else if (pv != kv.Value)
            {
                report.Add(AbiFindingKind.Error,
                    $"{kv.Key} value mismatch: header {Hex(kv.Value)}, prelude {Hex(pv)}");
            }
        }

        foreach (KeyValuePair<string, List<string>> fam in unmirrored)
        {
            report.Add(AbiFindingKind.Info,
                $"{fam.Key}* is not mirrored in the prelude ({fam.Value.Count} constant(s): " +
                $"{string.Join(", ", fam.Value)}). Userspace that needs these has to redeclare " +
                $"them - os/std/net.xfxn already redeclares NET_OP_* for exactly this reason, " +
                $"which is a third copy of the ABI with no link back to the header.");
        }

        if (unfamilied.Count > 0)
        {
            report.Add(AbiFindingKind.Info,
                $"{unfamilied.Count} header constant(s) belong to no known family and were not " +
                $"checked: {string.Join(", ", unfamilied)}. Add the prefix to AbiSync.Families " +
                $"if these are ABI surface.");
        }

        foreach (KeyValuePair<string, long> kv in Sorted(pConst))
        {
            string fam = FamilyOf(kv.Key);
            if (fam.Length == 0) continue;
            if (!hConst.ContainsKey(kv.Key))
            {
                report.Add(AbiFindingKind.Warning,
                    $"{kv.Key} = {Hex(kv.Value)} is in the prelude but not in the header " +
                    $"(stale, or the header lost it)");
            }
        }

        // ---- structs ----
        foreach (KeyValuePair<string, List<string>> kv in SortedStructs(hStructs))
        {
            if (!pStructs.TryGetValue(kv.Key, out List<string>? pf))
            {
                report.Add(AbiFindingKind.Error,
                    $"struct {kv.Key} is in the header but missing from the prelude " +
                    $"(fields: {string.Join(", ", kv.Value)})");
                continue;
            }

            List<string> hf = kv.Value;
            if (hf.Count != pf.Count)
            {
                report.Add(AbiFindingKind.Error,
                    $"struct {kv.Key} field count differs: header {hf.Count} " +
                    $"({string.Join(", ", hf)}), prelude {pf.Count} ({string.Join(", ", pf)})");
                continue;
            }

            for (int i = 0; i < hf.Count; i++)
            {
                if (!string.Equals(hf[i], pf[i], StringComparison.Ordinal))
                {
                    report.Add(AbiFindingKind.Error,
                        $"struct {kv.Key} field {i} differs: header '{hf[i]}', prelude '{pf[i]}' " +
                        $"- field order is the layout contract, so this silently corrupts every " +
                        $"call using this struct");
                }
            }
        }

        foreach (KeyValuePair<string, List<string>> kv in SortedStructs(pStructs))
        {
            if (!hStructs.ContainsKey(kv.Key))
            {
                report.Add(AbiFindingKind.Warning,
                    $"struct {kv.Key} is in the prelude but not in the header (stale?)");
            }
        }

        return report;
    }

    // ---- helpers: ordering (no LINQ dependency, keeps this file trivially portable) ----

    private static IEnumerable<KeyValuePair<string, long>> Sorted(Dictionary<string, long> d)
    {
        var keys = new List<string>(d.Keys);
        keys.Sort(StringComparer.Ordinal);
        foreach (string k in keys) yield return new KeyValuePair<string, long>(k, d[k]);
    }

    private static IEnumerable<KeyValuePair<string, List<string>>> SortedStructs(
        Dictionary<string, List<string>> d)
    {
        var keys = new List<string>(d.Keys);
        keys.Sort(StringComparer.Ordinal);
        foreach (string k in keys) yield return new KeyValuePair<string, List<string>>(k, d[k]);
    }

    private static string Hex(long v) =>
        v < 0 ? v.ToString(CultureInfo.InvariantCulture)
              : "0x" + v.ToString("X2", CultureInfo.InvariantCulture);

    private static string FamilyOf(string name)
    {
        foreach (string f in Families)
            if (name.StartsWith(f, StringComparison.Ordinal)) return f;
        return string.Empty;
    }

    // ---- helpers: parsing ----

    /// <summary>Removes C comments, preserving newlines so line structure survives.</summary>
    internal static string StripComments(string s)
    {
        var sb = new StringBuilder(s.Length);
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] == '/' && i + 1 < s.Length && s[i + 1] == '*')
            {
                int end = s.IndexOf("*/", i + 2, StringComparison.Ordinal);
                if (end < 0) break;
                for (int j = i; j < end + 2; j++)
                    if (s[j] == '\n') sb.Append('\n');
                i = end + 1;
                continue;
            }
            if (s[i] == '/' && i + 1 < s.Length && s[i + 1] == '/')
            {
                int end = s.IndexOf('\n', i);
                if (end < 0) break;
                i = end - 1;          // the loop's ++ lands on the newline, which is kept
                continue;
            }
            sb.Append(s[i]);
        }
        return sb.ToString();
    }

    /// <summary>
    /// Parses a C integer literal: decimal or hex, optional <c>u</c>/<c>l</c> suffixes,
    /// optional surrounding parentheses (the header writes negatives as <c>(-1)</c>).
    /// Expression-valued defines such as <c>CAP_OS_BASELINE</c> fail here and are skipped,
    /// which is intended - this checker compares values, not macro algebra.
    /// </summary>
    internal static bool TryParseNumber(string raw, out long value)
    {
        value = 0;
        string t = raw.Trim();

        while (t.Length > 1 && t[0] == '(' && t[t.Length - 1] == ')')
            t = t.Substring(1, t.Length - 2).Trim();

        bool negative = false;
        if (t.StartsWith("-", StringComparison.Ordinal))
        {
            negative = true;
            t = t.Substring(1).Trim();
        }

        while (t.Length > 0 &&
               (t[t.Length - 1] == 'u' || t[t.Length - 1] == 'U' ||
                t[t.Length - 1] == 'l' || t[t.Length - 1] == 'L'))
            t = t.Substring(0, t.Length - 1);

        if (t.Length == 0) return false;

        bool ok = t.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? long.TryParse(t.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out value)
            : long.TryParse(t, NumberStyles.Integer, CultureInfo.InvariantCulture, out value);

        if (ok && negative) value = -value;
        return ok;
    }

    private static Dictionary<string, long> ParseCDefines(string text)
    {
        var result = new Dictionary<string, long>(StringComparer.Ordinal);
        foreach (string rawLine in text.Split('\n'))
        {
            string line = rawLine.Trim();
            if (!line.StartsWith("#define", StringComparison.Ordinal)) continue;

            string rest = line.Substring("#define".Length).Trim();
            int sp = rest.IndexOfAny(new[] { ' ', '\t' });
            if (sp <= 0) continue;                       // bare guard, e.g. #define CXK_ABI_H

            string name = rest.Substring(0, sp);
            if (name.IndexOf('(') >= 0) continue;        // function-like macro

            if (TryParseNumber(rest.Substring(sp + 1), out long v)) result[name] = v;
        }
        return result;
    }

    /// <summary>Adds members of C enums (the header uses anonymous enums for op selectors).</summary>
    private static void AddCEnumMembers(string text, Dictionary<string, long> into)
    {
        foreach (Match m in Regex.Matches(
                     text, @"enum\s*(?:[A-Za-z_][A-Za-z0-9_]*\s*)?\{([^}]*)\}", RegexOptions.Singleline))
        {
            long next = 0;
            foreach (string piece in m.Groups[1].Value.Split(','))
            {
                string p = piece.Trim();
                if (p.Length == 0) continue;

                string name;
                long value;
                int eq = p.IndexOf('=');
                if (eq > 0)
                {
                    name = p.Substring(0, eq).Trim();
                    if (!TryParseNumber(p.Substring(eq + 1), out value)) value = next;
                }
                else
                {
                    name = p;
                    value = next;
                }

                if (!Regex.IsMatch(name, @"^[A-Za-z_][A-Za-z0-9_]*$")) continue;
                into[name] = value;
                next = value + 1;
            }
        }
    }

    private static Dictionary<string, long> ParseXConsts(string text)
    {
        var result = new Dictionary<string, long>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(
                     text,
                     @"^[ \t]*const\s+([A-Za-z_][A-Za-z0-9_]*)\s*:\s*[A-Za-z0-9_]+\s*=\s*([^;]+);",
                     RegexOptions.Multiline))
        {
            if (TryParseNumber(m.Groups[2].Value, out long v)) result[m.Groups[1].Value] = v;
        }
        return result;
    }

    private static Dictionary<string, List<string>> ParseCStructs(string text)
    {
        var result = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(
                     text, @"struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([^}]*)\}", RegexOptions.Singleline))
        {
            var fields = new List<string>();
            foreach (string decl in m.Groups[2].Value.Split(';'))
            {
                if (decl.Trim().Length == 0) continue;
                // A single declaration may name several fields: `uint32_t x, y;`
                foreach (string part in decl.Split(','))
                {
                    string name = LastIdentifier(part);
                    if (name.Length > 0) fields.Add(name);
                }
            }
            result[m.Groups[1].Value] = fields;
        }
        return result;
    }

    private static Dictionary<string, List<string>> ParseXStructs(string text)
    {
        var result = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        foreach (Match m in Regex.Matches(
                     text, @"^[ \t]*struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([^}]*)\}",
                     RegexOptions.Multiline | RegexOptions.Singleline))
        {
            var fields = new List<string>();
            foreach (string f in m.Groups[2].Value.Split(','))
            {
                string t = f.Trim();
                if (t.Length == 0) continue;
                int colon = t.IndexOf(':');
                if (colon <= 0) continue;
                fields.Add(t.Substring(0, colon).Trim());
            }
            result[m.Groups[1].Value] = fields;
        }
        return result;
    }

    /// <summary>
    /// The declared name in a C declarator: the trailing identifier once any array suffix
    /// is dropped. Handles <c>uint32_t image_len</c>, <c>const void *image</c>,
    /// <c>uint32_t *out</c> and the second half of <c>int32_t x, y</c>.
    /// </summary>
    internal static string LastIdentifier(string declarator)
    {
        string d = declarator.Trim();

        int bracket = d.IndexOf('[');
        if (bracket >= 0) d = d.Substring(0, bracket);
        d = d.TrimEnd();

        int end = d.Length;
        while (end > 0 && !(char.IsLetterOrDigit(d[end - 1]) || d[end - 1] == '_')) end--;
        int start = end;
        while (start > 0 && (char.IsLetterOrDigit(d[start - 1]) || d[start - 1] == '_')) start--;

        return end > start ? d.Substring(start, end - start) : string.Empty;
    }
}

/// <summary>Severity of a single <see cref="AbiSync"/> finding.</summary>
public enum AbiFindingKind
{
    /// <summary>The prelude and header disagree in a way that will break a build or corrupt calls.</summary>
    Error,
    /// <summary>The prelude carries something the header does not - probably stale.</summary>
    Warning,
    /// <summary>Not a defect: header surface the prelude intentionally does not mirror.</summary>
    Info,
}

/// <summary>A single difference between the header and the prelude.</summary>
public sealed record AbiFinding(AbiFindingKind Kind, string Message);

/// <summary>The result of an <see cref="AbiSync.Compare"/> run.</summary>
public sealed class AbiSyncReport
{
    public List<AbiFinding> Findings { get; } = new();

    public int HeaderConstants { get; internal set; }
    public int PreludeConstants { get; internal set; }
    public int HeaderStructs { get; internal set; }
    public int PreludeStructs { get; internal set; }

    /// <summary>True when nothing that would break a build was found.</summary>
    public bool Ok => Count(AbiFindingKind.Error) == 0;

    public int Count(AbiFindingKind kind)
    {
        int n = 0;
        foreach (AbiFinding f in Findings)
            if (f.Kind == kind) n++;
        return n;
    }

    internal void Add(AbiFindingKind kind, string message) =>
        Findings.Add(new AbiFinding(kind, message));
}
