// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace CXEX.Build.Locale;

/// <summary>
/// The subset of Mozilla's Fluent syntax CXOS's string tables are written in
/// (system/CX_TOOLKIT.md §10). We follow the design, not its code: comments,
/// messages, attributes, text over several lines, variables, string
/// literals, and a select on a number whose keys are CLDR plural categories
/// or integers. Everything else Fluent has (message references, terms,
/// functions, selects on strings) is refused with its line named, never
/// passed through as text.
/// </summary>
public static class Fluent
{
    public abstract record Element;
    public sealed record Text(string Value) : Element;
    public sealed record Var(string Name) : Element;
    public sealed record Variant(bool IsNumber, int Category, int Number, bool IsDefault, List<Element> Pattern);
    public sealed record Select(string Name, List<Variant> Variants) : Element;

    /// <summary>A message (or an attribute, keyed `message.attr`) and where it was written.</summary>
    public sealed record Message(string Key, List<Element> Pattern, string Where);

    public sealed class FluentException : Exception
    {
        public FluentException(string where, string what) : base($"{where}: {what}") { }
    }

    public static bool IsId(string s) =>
        s.Length > 0 && char.IsAsciiLetter(s[0]) && s.All(c => char.IsAsciiLetterOrDigit(c) || c == '-' || c == '_');

    public static List<Message> ParseFile(string path) => Parse(File.ReadAllText(path), Path.GetFileName(path));

    public static List<Message> Parse(string source, string name)
    {
        string[] lines = source.Replace("\r\n", "\n").Split('\n');
        var msgs = new List<Message>();
        int n = 0;
        while (n < lines.Length)
        {
            string line = lines[n];
            if (line.Length == 0 || line[0] == '#') { n++; continue; }
            string where = $"{name}:{n + 1}";
            if (line[0] == ' ' || line[0] == '\t')
            {
                if (line.Trim().Length == 0) { n++; continue; }
                throw new FluentException(where, "an indented line with no message before it");
            }
            if (line[0] == '-') throw new FluentException(where, "terms (-name) are not in CXOS's subset");
            int eq = line.IndexOf('=');
            if (eq < 0) throw new FluentException(where, "expected 'key = text'");
            string key = line[..eq].Trim();
            if (!IsId(key)) throw new FluentException(where, $"'{key}' is not a key: a letter, then letters, digits, - and _");
            // the message's own lines: the first's rest, and every line after
            // that is indented or blank (a blank one only if an indented one follows)
            var body = new List<(string Text, int Line)> { (line[(eq + 1)..], n + 1) };
            int m = n + 1;
            while (m < lines.Length)
            {
                string l = lines[m];
                if (l.Length > 0 && l[0] != ' ' && l[0] != '\t') break;
                if (l.Trim().Length == 0)
                {
                    int k = m + 1;
                    while (k < lines.Length && lines[k].Trim().Length == 0) k++;
                    if (k >= lines.Length || (lines[k].Length > 0 && lines[k][0] != ' ' && lines[k][0] != '\t')) break;
                }
                body.Add((l, m + 1));
                m++;
            }
            n = m;
            // split off the attributes: a line, outside braces, whose text starts ".id ="
            var parts = new List<(string Key, List<(string, int)> Lines)> { (key, new List<(string, int)>()) };
            int depth = 0;
            foreach (var (text, ln) in body)
            {
                string t = text.TrimStart();
                if (depth == 0 && ln != body[0].Line && t.StartsWith('.'))
                {
                    int e2 = t.IndexOf('=');
                    string attr = e2 < 0 ? "" : t[1..e2].Trim();
                    if (e2 < 0 || !IsId(attr)) throw new FluentException($"{name}:{ln}", "expected '.attribute = text'");
                    parts.Add(($"{key}.{attr}", new List<(string, int)> { (t[(e2 + 1)..], ln) }));
                    continue;
                }
                parts[^1].Lines.Add((text, ln));
                foreach (char c in text) { if (c == '{') depth++; else if (c == '}') depth--; }
            }
            foreach (var (k, ls) in parts)
            {
                string w = $"{name}:{ls[0].Item2}";
                var p = new PatternParser(ls, name).Top();
                // a message may be only its attributes (Fluent's `key =` then `.attr = ...`)
                if (p.Count == 0 && k == key && parts.Count > 1) continue;
                if (p.Count == 0) throw new FluentException(w, $"'{k}' has no text");
                msgs.Add(new Message(k, p, w));
            }
        }
        return msgs;
    }

    /// <summary>
    /// Reads one message's lines as one string, its line breaks kept and each
    /// continuation line's indent dropped (outside braces), then the pattern.
    /// </summary>
    private sealed class PatternParser
    {
        private readonly string s;
        private readonly int[] lineAt;      // the source line of each character
        private readonly string file;
        private int at;

        public PatternParser(List<(string Text, int Line)> lines, string file)
        {
            this.file = file;
            var sb = new StringBuilder();
            var la = new List<int>();
            int depth = 0;
            for (int k = 0; k < lines.Count; k++)
            {
                string t = lines[k].Text;
                if (k > 0) { sb.Append('\n'); la.Add(lines[k].Line); }
                // the first line's text starts after '= ' and its blank; others lose their indent
                if (k == 0 || depth == 0) t = t.TrimStart(' ', '\t');
                foreach (char c in t) { sb.Append(c); la.Add(lines[k].Line); if (c == '{') depth++; else if (c == '}') depth--; }
            }
            s = sb.ToString();
            lineAt = la.ToArray();
            // a value that starts on the next line has no leading line break
            while (at < s.Length && s[at] == '\n') at++;
        }

        private string Where => $"{file}:{(at < lineAt.Length ? lineAt[at] : (lineAt.Length > 0 ? lineAt[^1] : 0))}";
        private FluentException Error(string what) => new(Where, what);

        public List<Element> Top()
        {
            var p = Pattern(false);
            if (at < s.Length) throw Error($"unexpected '{s[at]}'");
            return Tidy(p);
        }

        // Text and placeables, until the end, or (in a variant) a line whose
        // first character is '[', '*' or '}'.
        private List<Element> Pattern(bool inVariant)
        {
            var o = new List<Element>();
            var text = new StringBuilder();
            while (at < s.Length)
            {
                char c = s[at];
                if (c == '}') { if (inVariant) break; throw Error("a '}' with no '{' (write { \"}\" } for one)"); }
                if (c == '\n' && inVariant)
                {
                    int k = at + 1;
                    while (k < s.Length && (s[k] == ' ' || s[k] == '\t')) k++;
                    if (k >= s.Length || s[k] == '[' || s[k] == '*' || s[k] == '}') break;
                    // a continuation inside a variant: its indent goes
                    text.Append('\n');
                    at = k;
                    continue;
                }
                if (c == '{')
                {
                    at++;
                    var e = Placeable();
                    if (e is Text lit) { text.Append(lit.Value); continue; }
                    if (text.Length > 0) { o.Add(new Text(text.ToString())); text.Clear(); }
                    o.Add(e);
                    continue;
                }
                text.Append(c);
                at++;
            }
            if (text.Length > 0) o.Add(new Text(text.ToString()));
            return o;
        }

        private void Blank() { while (at < s.Length && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n')) at++; }

        private string Id()
        {
            int a = at;
            while (at < s.Length && (char.IsAsciiLetterOrDigit(s[at]) || s[at] == '-' || s[at] == '_')) at++;
            string id = s[a..at];
            if (!IsId(id)) throw Error($"expected a name, got '{id}'");
            return id;
        }

        private Element Placeable()
        {
            Blank();
            if (at >= s.Length) throw Error("a '{' that is never closed");
            Element e;
            if (s[at] == '"') e = new Text(StringLiteral());
            else if (s[at] == '$')
            {
                at++;
                string name = Id();
                Blank();
                if (at + 1 < s.Length && s[at] == '-' && s[at + 1] == '>') { at += 2; e = SelectBody(name); }
                else e = new Var(name);
            }
            else if (s[at] == '-') throw Error("terms (-name) are not in CXOS's subset");
            else if (char.IsAsciiLetter(s[at]))
            {
                string id = Id();
                Blank();
                if (at < s.Length && s[at] == '(') throw Error($"functions ({id}(...)) are not in CXOS's subset");
                throw Error($"message references ({{ {id} }}) are not in CXOS's subset");
            }
            else throw Error($"'{s[at]}' cannot start a placeable");
            Blank();
            if (at >= s.Length || s[at] != '}') throw Error("expected '}'");
            at++;
            return e;
        }

        private string StringLiteral()
        {
            at++;
            var sb = new StringBuilder();
            while (true)
            {
                if (at >= s.Length || s[at] == '\n') throw Error("a string literal that is never closed");
                char c = s[at++];
                if (c == '"') break;
                if (c != '\\') { sb.Append(c); continue; }
                if (at >= s.Length) throw Error("a '\\' at the end");
                char d = s[at++];
                if (d == '"' || d == '\\') sb.Append(d);
                else if (d == 'u' || d == 'U')
                {
                    int len = d == 'u' ? 4 : 6;
                    if (at + len > s.Length) throw Error("a short \\u escape");
                    int cp = int.Parse(s.AsSpan(at, len), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) throw Error($"\\u{cp:X} is not a character");
                    sb.Append(char.ConvertFromUtf32(cp));
                    at += len;
                }
                else throw Error($"unknown escape '\\{d}'");
            }
            return sb.ToString();
        }

        private Select SelectBody(string name)
        {
            var vs = new List<Variant>();
            while (true)
            {
                Blank();
                if (at >= s.Length) throw Error("a select that is never closed");
                if (s[at] == '}') break;
                bool def = false;
                if (s[at] == '*') { def = true; at++; }
                if (at >= s.Length || s[at] != '[') throw Error("expected a variant, '[key] text'");
                at++;
                Blank();
                int a = at;
                while (at < s.Length && s[at] != ']' && s[at] != '\n') at++;
                string key = s[a..at].Trim();
                if (at >= s.Length || s[at] != ']') throw Error("expected ']'");
                at++;
                Variant v;
                if (key.Length > 0 && (char.IsAsciiDigit(key[0]) || key[0] == '-'))
                {
                    if (!int.TryParse(key, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out int num))
                        throw Error($"variant key '{key}' is not an integer");
                    v = new Variant(true, 0, num, def, null!);
                }
                else
                {
                    int cat = Array.IndexOf(Plurals.Categories, key);
                    if (cat < 0) throw Error($"variant key '{key}' is neither a plural category ({string.Join(", ", Plurals.Categories)}) nor an integer: selects are on numbers only");
                    v = new Variant(false, cat, 0, def, null!);
                }
                while (at < s.Length && (s[at] == ' ' || s[at] == '\t')) at++;
                var p = Tidy(Pattern(true));
                vs.Add(v with { Pattern = p });
            }
            int defaults = vs.Count(v => v.IsDefault);
            if (defaults != 1) throw Error($"a select needs exactly one default variant (*[...]); this one has {defaults}");
            var seen = new HashSet<(bool, int)>();
            foreach (var v in vs)
                if (!seen.Add((v.IsNumber, v.IsNumber ? v.Number : v.Category)))
                    throw Error("a variant key given twice");
            return new Select(name, vs);
        }

        // Adjacent text merged, the last text's trailing blanks and line breaks dropped.
        private static List<Element> Tidy(List<Element> p)
        {
            var o = new List<Element>();
            foreach (var e in p)
            {
                if (e is Text t && o.Count > 0 && o[^1] is Text u) o[^1] = new Text(u.Value + t.Value);
                else o.Add(e);
            }
            if (o.Count > 0 && o[^1] is Text last)
            {
                string v = last.Value.TrimEnd(' ', '\t', '\n');
                if (v.Length == 0) o.RemoveAt(o.Count - 1); else o[^1] = new Text(v);
            }
            return o;
        }
    }

    /// <summary>The variables a pattern uses, nested selects' too.</summary>
    public static HashSet<string> Variables(List<Element> p)
    {
        var s = new HashSet<string>(StringComparer.Ordinal);
        foreach (var e in p)
        {
            if (e is Var v) s.Add(v.Name);
            else if (e is Select sel) { s.Add(sel.Name); foreach (var x in sel.Variants) s.UnionWith(Variables(x.Pattern)); }
        }
        return s;
    }

    /// <summary>Every select in a pattern, nested ones too.</summary>
    public static IEnumerable<Select> Selects(List<Element> p)
    {
        foreach (var e in p)
            if (e is Select sel)
            {
                yield return sel;
                foreach (var v in sel.Variants) foreach (var inner in Selects(v.Pattern)) yield return inner;
            }
    }
}
