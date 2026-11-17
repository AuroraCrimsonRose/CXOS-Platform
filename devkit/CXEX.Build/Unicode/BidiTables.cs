// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace CXEX.Build.Unicode;

/// <summary>
/// The tables UAX #9 needs, from the Unicode Character Database's own files
/// (vendor/unicode), written as an X source file (os/std/unibidi.xfxn) that
/// os/std/bidi.xfxn searches. The generated file is checked in, so the OS
/// build needs no generator step; a DevKit test regenerates it from the
/// vendored files and requires the same bytes, as `cxk check-abi` does for
/// the ABI header (system/CX_TOOLKIT.md §10).
///
/// Three tables: every code point's Bidi_Class as runs (each entry the first
/// code point of a run and its class, packed `start &lt;&lt; 5 | class`, so a
/// binary search finds the run), the paired brackets (BidiBrackets.txt), and
/// the mirrored glyphs (BidiMirroring.txt).
/// </summary>
public static class BidiTables
{
    /// <summary>The classes in the order bidi.xfxn numbers them (BD_L = 0 ...).</summary>
    public static readonly string[] Classes =
    {
        "L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON",
        "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI",
    };

    // The long names @missing lines use.
    private static readonly Dictionary<string, string> LongNames = new(StringComparer.Ordinal)
    {
        ["Left_To_Right"] = "L", ["Right_To_Left"] = "R", ["Arabic_Letter"] = "AL",
        ["European_Number"] = "EN", ["European_Separator"] = "ES", ["European_Terminator"] = "ET",
        ["Arabic_Number"] = "AN", ["Common_Separator"] = "CS", ["Nonspacing_Mark"] = "NSM",
        ["Boundary_Neutral"] = "BN", ["Paragraph_Separator"] = "B", ["Segment_Separator"] = "S",
        ["White_Space"] = "WS", ["Other_Neutral"] = "ON",
        ["Left_To_Right_Embedding"] = "LRE", ["Left_To_Right_Override"] = "LRO",
        ["Right_To_Left_Embedding"] = "RLE", ["Right_To_Left_Override"] = "RLO",
        ["Pop_Directional_Format"] = "PDF", ["Left_To_Right_Isolate"] = "LRI",
        ["Right_To_Left_Isolate"] = "RLI", ["First_Strong_Isolate"] = "FSI",
        ["Pop_Directional_Isolate"] = "PDI",
    };

    public const int CodeSpace = 0x110000;

    public sealed record Tables(
        string Version,
        byte[] ClassOf,                      // one per code point
        List<(int Start, int Class)> Runs,
        List<(int Cp, int Pair, bool Open)> Brackets,
        List<(int Cp, int To)> Mirrors);

    private static int ClassIndex(string name, string where)
    {
        int k = Array.IndexOf(Classes, name);
        if (k < 0 && LongNames.TryGetValue(name, out var s)) k = Array.IndexOf(Classes, s);
        if (k < 0) throw new InvalidDataException($"{where}: unknown Bidi_Class '{name}'");
        return k;
    }

    private static (int Lo, int Hi) Range(string field, string where)
    {
        field = field.Trim();
        int dots = field.IndexOf("..", StringComparison.Ordinal);
        int lo = int.Parse(dots < 0 ? field : field[..dots], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        int hi = dots < 0 ? lo : int.Parse(field[(dots + 2)..], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        if (lo < 0 || hi >= CodeSpace || hi < lo) throw new InvalidDataException($"{where}: bad range '{field}'");
        return (lo, hi);
    }

    private static string VersionOf(string firstLine, string file)
    {
        // "# DerivedBidiClass-18.0.0.txt"
        int dash = firstLine.LastIndexOf('-');
        int dot = firstLine.LastIndexOf(".txt", StringComparison.Ordinal);
        if (!firstLine.StartsWith("# ", StringComparison.Ordinal) || dash < 0 || dot < dash)
            throw new InvalidDataException($"{file}: the first line does not name the file and its version");
        return firstLine[(dash + 1)..dot];
    }

    /// <summary>Reads the three files from a directory holding them.</summary>
    public static Tables Load(string dir)
    {
        string classPath = Path.Combine(dir, "DerivedBidiClass.txt");
        string[] lines = File.ReadAllLines(classPath);
        string version = VersionOf(lines[0], classPath);
        var cls = new byte[CodeSpace];
        bool[] given = new bool[CodeSpace];
        bool sawMissing = false;
        for (int n = 0; n < lines.Length; n++)
        {
            string where = $"DerivedBidiClass.txt:{n + 1}";
            string line = lines[n];
            if (line.StartsWith("# @missing:", StringComparison.Ordinal))
            {
                // "# @missing: 0590..05FF; Right_To_Left": later lines override earlier
                string body = line["# @missing:".Length..];
                string[] f = body.Split(';');
                var (lo, hi) = Range(f[0], where);
                int k = ClassIndex(f[1].Trim(), where);
                for (int cp = lo; cp <= hi; cp++) cls[cp] = (byte)k;
                sawMissing = true;
                continue;
            }
            int hash = line.IndexOf('#');
            string data = (hash < 0 ? line : line[..hash]).Trim();
            if (data.Length == 0) continue;
            string[] g = data.Split(';');
            if (g.Length != 2) throw new InvalidDataException($"{where}: expected 'range ; class'");
            var (a, b) = Range(g[0], where);
            int c = ClassIndex(g[1].Trim(), where);
            for (int cp = a; cp <= b; cp++)
            {
                if (given[cp]) throw new InvalidDataException($"{where}: U+{cp:X4} given twice");
                given[cp] = true;
                cls[cp] = (byte)c;
            }
        }
        if (!sawMissing) throw new InvalidDataException("DerivedBidiClass.txt: no @missing lines (the defaults for unassigned code points)");

        var runs = new List<(int, int)>();
        for (int cp = 0; cp < CodeSpace; cp++)
            if (cp == 0 || cls[cp] != cls[cp - 1]) runs.Add((cp, cls[cp]));

        string brPath = Path.Combine(dir, "BidiBrackets.txt");
        string[] br = File.ReadAllLines(brPath);
        if (VersionOf(br[0], brPath) != version) throw new InvalidDataException("BidiBrackets.txt is not the same Unicode version");
        var brackets = new List<(int, int, bool)>();
        for (int n = 0; n < br.Length; n++)
        {
            string where = $"BidiBrackets.txt:{n + 1}";
            int hash = br[n].IndexOf('#');
            string data = (hash < 0 ? br[n] : br[n][..hash]).Trim();
            if (data.Length == 0) continue;
            string[] f = data.Split(';');
            if (f.Length != 3) throw new InvalidDataException($"{where}: expected 'cp; pair; type'");
            int cp = Range(f[0], where).Lo;
            int pair = Range(f[1], where).Lo;
            string t = f[2].Trim();
            if (t != "o" && t != "c") throw new InvalidDataException($"{where}: type '{t}' is neither o nor c");
            brackets.Add((cp, pair, t == "o"));
        }
        brackets.Sort((x, y) => x.Item1.CompareTo(y.Item1));

        string mPath = Path.Combine(dir, "BidiMirroring.txt");
        string[] mr = File.ReadAllLines(mPath);
        if (VersionOf(mr[0], mPath) != version) throw new InvalidDataException("BidiMirroring.txt is not the same Unicode version");
        var mirrors = new List<(int, int)>();
        for (int n = 0; n < mr.Length; n++)
        {
            string where = $"BidiMirroring.txt:{n + 1}";
            int hash = mr[n].IndexOf('#');
            string data = (hash < 0 ? mr[n] : mr[n][..hash]).Trim();
            if (data.Length == 0) continue;
            string[] f = data.Split(';');
            if (f.Length != 2) throw new InvalidDataException($"{where}: expected 'cp; mirror'");
            mirrors.Add((Range(f[0], where).Lo, Range(f[1], where).Lo));
        }
        mirrors.Sort((x, y) => x.Item1.CompareTo(y.Item1));
        for (int k = 1; k < mirrors.Count; k++)
            if (mirrors[k].Item1 == mirrors[k - 1].Item1) throw new InvalidDataException($"BidiMirroring.txt: U+{mirrors[k].Item1:X4} twice");

        return new Tables(version, cls, runs, brackets, mirrors);
    }

    /// <summary>The X source os/std/unibidi.xfxn holds. Deterministic: the same files give the same bytes.</summary>
    public static string EmitX(Tables t)
    {
        var sb = new StringBuilder();
        sb.Append("// SPDX-License-Identifier: MIT AND Unicode-3.0\n");
        sb.Append("// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)\n");
        sb.Append("// SPDX-FileCopyrightText: 1991-2026 Unicode, Inc. (the data)\n");
        sb.Append("// std/unibidi.xfxn - GENERATED by `cxk unicode bidi vendor/unicode -o os/std/unibidi.xfxn`.\n");
        sb.Append("// Do not edit: a DevKit test regenerates it from the vendored files and\n");
        sb.Append("// requires these bytes. The tables of std/bidi.xfxn (system/CX_TOOLKIT.md §10).\n");
        sb.Append($"// Unicode {t.Version}: DerivedBidiClass.txt, BidiBrackets.txt, BidiMirroring.txt.\n");
        sb.Append("//\n");
        sb.Append("// ub_class: each entry the first code point of a run and its class,\n");
        sb.Append("// start << 5 | class, rising; a code point's class is its run's.\n");
        sb.Append("// ub_brk_cp rising, with ub_brk_pair the bracket it pairs with, the top\n");
        sb.Append("// bit set for an opening one. ub_mir_cp rising, ub_mir_to its mirror.\n\n");
        XArray(sb, "UB_CLASS_N", "ub_class", t.Runs.ConvertAll(r => (uint)((r.Start << 5) | r.Class)));
        XArray(sb, "UB_BRK_N", "ub_brk_cp", t.Brackets.ConvertAll(b => (uint)b.Cp));
        XArray(sb, "UB_BRK_N", "ub_brk_pair", t.Brackets.ConvertAll(b => (uint)b.Pair | (b.Open ? 0x80000000u : 0u)), constant: false);
        XArray(sb, "UB_MIR_N", "ub_mir_cp", t.Mirrors.ConvertAll(m => (uint)m.Cp));
        XArray(sb, "UB_MIR_N", "ub_mir_to", t.Mirrors.ConvertAll(m => (uint)m.To), constant: false);
        return sb.ToString();
    }

    private static void XArray(StringBuilder sb, string countName, string name, List<uint> v, bool constant = true)
    {
        if (constant) sb.Append($"const {countName}: u32 = {v.Count};\n");
        sb.Append($"global {name}: [{v.Count}]u32 = [\n");
        for (int k = 0; k < v.Count; k++)
        {
            if (k % 8 == 0) sb.Append("    ");
            sb.Append("0x").Append(v[k].ToString("X8", CultureInfo.InvariantCulture));
            if (k != v.Count - 1) sb.Append(',');
            sb.Append(k % 8 == 7 || k == v.Count - 1 ? "\n" : " ");
        }
        sb.Append("];\n\n");
    }
}
