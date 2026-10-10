// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace CXEX.Build.Fonts;

/// <summary>
/// The AngelCode BMFont TEXT descriptor: `info face="..." size=8 ...` then one
/// `char id=... x=... y=...` line per glyph. Only the fields that decide where
/// a glyph's pixels are and how big its cell is are read; the rest of the
/// format is ignored rather than half-supported.
///
/// Read on the host only. The kernel never sees this.
/// </summary>
public sealed class BmFont
{
    public string Face { get; private init; } = "";
    public int Size { get; private init; }
    public int LineHeight { get; private init; }
    public string PageFile { get; private init; } = "";
    public List<BmGlyph> Glyphs { get; } = new();

    public sealed record BmGlyph(uint Id, int X, int Y, int Width, int Height,
                                 int XOffset, int YOffset, int XAdvance, int Page);

    public static BmFont Load(string path)
    {
        var font = Parse(File.ReadAllText(path));
        return font;
    }

    public static BmFont Parse(string text)
    {
        string face = "", page = "";
        int size = 0, lineHeight = 0;
        var glyphs = new List<BmGlyph>();

        foreach (var rawLine in text.Split('\n'))
        {
            string line = rawLine.Trim('\r', ' ', '\t');
            if (line.Length == 0) continue;

            int sp = line.IndexOf(' ');
            string tag = sp < 0 ? line : line[..sp];
            var kv = ParseKeyValues(sp < 0 ? "" : line[(sp + 1)..]);

            switch (tag)
            {
                case "info":
                    face = kv.TryGetValue("face", out var f) ? f.Trim('"') : "";
                    size = GetInt(kv, "size", 0);
                    break;
                case "common":
                    lineHeight = GetInt(kv, "lineHeight", 0);
                    break;
                case "page":
                    if (GetInt(kv, "id", 0) == 0 && kv.TryGetValue("file", out var pf))
                        page = pf.Trim('"');
                    break;
                case "char":
                    glyphs.Add(new BmGlyph(
                        (uint)GetInt(kv, "id", 0),
                        GetInt(kv, "x", 0), GetInt(kv, "y", 0),
                        GetInt(kv, "width", 0), GetInt(kv, "height", 0),
                        GetInt(kv, "xoffset", 0), GetInt(kv, "yoffset", 0),
                        GetInt(kv, "xadvance", 0), GetInt(kv, "page", 0)));
                    break;
            }
        }

        if (glyphs.Count == 0) throw new InvalidDataException("no `char` lines: not a BMFont text descriptor?");
        if (page.Length == 0) throw new InvalidDataException("no `page id=0 file=...` line: nowhere to read pixels from");

        var bm = new BmFont { Face = face, Size = size, LineHeight = lineHeight, PageFile = page };
        bm.Glyphs.AddRange(glyphs);
        return bm;
    }

    /// <summary>
    /// Splits `a=1 b="two words" c=3,4` into pairs. Quoted values may contain
    /// spaces, which is the only reason this is not a plain split.
    /// </summary>
    private static Dictionary<string, string> ParseKeyValues(string s)
    {
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        int i = 0;
        while (i < s.Length)
        {
            while (i < s.Length && s[i] == ' ') i++;
            if (i >= s.Length) break;

            int eq = s.IndexOf('=', i);
            if (eq < 0) break;
            string key = s[i..eq].Trim();

            int v = eq + 1;
            string val;
            if (v < s.Length && s[v] == '"')
            {
                int close = s.IndexOf('"', v + 1);
                if (close < 0) { val = s[v..]; i = s.Length; }
                else { val = s[v..(close + 1)]; i = close + 1; }
            }
            else
            {
                int end = s.IndexOf(' ', v);
                if (end < 0) end = s.Length;
                val = s[v..end];
                i = end;
            }
            map[key] = val;
        }
        return map;
    }

    private static int GetInt(Dictionary<string, string> kv, string key, int fallback)
        => kv.TryGetValue(key, out var s) &&
           int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out int n)
           ? n : fallback;
}
