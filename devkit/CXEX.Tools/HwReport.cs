// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text;
using System.Text.RegularExpressions;

namespace CXEX.Tools;

/// <summary>One line of the hardware report: its section and its fields.</summary>
public sealed record HwLine(string Section, IReadOnlyList<string> Fields);

/// <summary>
/// The kernel's hardware report (roadmap 1.8, docs/hardware/CX_HW_REPORT.md;
/// the form is kernel/diag/hwfmt.h): <c>@hw</c> lines, tab-separated, from
/// <c>@hw begin 1</c> to <c>@hw end &lt;lines&gt; &lt;crc&gt;</c>, the count
/// of lines before the end line and the CRC-32 of their bytes, each line
/// ending in LF. On serial each line also gets a CR and may land after half
/// of another line, so it is matched where it starts; in the crash area's
/// record (XCRL kind 6) it is the bare text.
/// </summary>
public sealed record HwReport(string Text, IReadOnlyList<HwLine> Lines)
{
    public const int FormVersion = 1;
    private static readonly Regex Tagged = new(@"@hw\t[^\r\n]*");

    /// <summary>The lines of one section, in order.</summary>
    public IEnumerable<HwLine> Section(string name) => Lines.Where(l => l.Section == name);

    /// <summary>Whether the report ran out of room in the kernel and was cut.</summary>
    public bool Truncated => Lines.Any(l => l.Section == "truncated");

    /// <summary>
    /// The machine's report in a serial log: from its last <c>@hw begin</c>
    /// through the first end line after it, rebuilt as the kernel's text, a
    /// profile (<see cref="ProfileFromSerial"/>) passed over. Null when the
    /// log holds no such begin line. The text may still fail <see cref="Check"/>.
    /// </summary>
    public static string? TextFromSerial(string log) => LastFromSerial(log, profile: false);

    /// <summary>
    /// The self-tests' profile in a serial log (1.9f): a report in the same
    /// form whose first line after the begin is <c>profile</c>, printed after
    /// the suite with the counted locks' contention. The last one, as
    /// <see cref="TextFromSerial"/> takes; null when there is none.
    /// </summary>
    public static string? ProfileFromSerial(string log) => LastFromSerial(log, profile: true);

    private static string? LastFromSerial(string log, bool profile)
    {
        string? found = null;
        List<string>? lines = null;
        void Finish()
        {
            if (lines is null || lines.Count == 0) return;
            bool isProfile = lines.Count > 1 && lines[1].StartsWith("@hw\tprofile", StringComparison.Ordinal);
            if (isProfile == profile) found = string.Join('\n', lines) + "\n";
        }
        foreach (string raw in log.Split('\n'))
        {
            var m = Tagged.Match(raw.TrimEnd('\r'));
            if (!m.Success) continue;
            if (m.Value.StartsWith("@hw\tbegin", StringComparison.Ordinal)) { Finish(); lines = []; }
            if (lines is null) continue;
            lines.Add(m.Value);
            if (m.Value.StartsWith("@hw\tend", StringComparison.Ordinal)) { Finish(); lines = null; }
        }
        Finish();   // cut off before its end line: taken as far as it goes
        return found;
    }

    /// <summary>The report's text checked and split into lines; throws <see cref="FormatException"/> on one that fails <see cref="Check"/>.</summary>
    public static HwReport Parse(string text)
    {
        if (Check(text) is string wrong) throw new FormatException("hardware report: " + wrong);
        var lines = text.TrimEnd('\n').Split('\n')
            .Select(l => l.Split('\t'))
            .Select(f => new HwLine(f[1], f[2..]))
            .ToList();
        return new HwReport(text, lines);
    }

    /// <summary>
    /// Null when the text is a whole report, as hw_check in the kernel judges
    /// it; otherwise what is wrong with it.
    /// </summary>
    public static string? Check(string text)
    {
        if (!text.StartsWith($"@hw\tbegin\t{FormVersion}\n", StringComparison.Ordinal))
            return $"it does not start with \"@hw begin {FormVersion}\"";
        if (!text.EndsWith('\n')) return "its last line has no newline";
        var lines = text[..^1].Split('\n');
        foreach (string l in lines)
            if (!l.StartsWith("@hw\t", StringComparison.Ordinal) || l.Contains('\r') || l.Contains('\0'))
                return $"a line is not an @hw line: \"{Clip(l)}\"";
        var end = Regex.Match(lines[^1], @"^@hw\tend\t(\d{1,10})\t([0-9a-f]{8})$");
        if (!end.Success) return $"it does not end with an end line: \"{Clip(lines[^1])}\"";
        if (!uint.TryParse(end.Groups[1].Value, out uint count) || count != lines.Length - 1)
            return $"its end line counts {end.Groups[1].Value} lines, but {lines.Length - 1} come before it";
        int body = text.Length - lines[^1].Length - 1;
        uint crc = Crc32(Encoding.ASCII.GetBytes(text[..body]));
        if (crc.ToString("x8") != end.Groups[2].Value)
            return $"its CRC is {end.Groups[2].Value}, but its lines' is {crc:x8}";
        return null;
    }

    private static string Clip(string s) => s.Length <= 60 ? s : s[..60] + "...";

    /// <summary>CRC-32 (IEEE, reflected), as xcrl_crc32.</summary>
    public static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint crc = ~0u;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }

    /// <summary>A report's text from lines without the begin and end, sealed as the kernel seals one (for tests and tools).</summary>
    public static string Seal(IEnumerable<string> body)
    {
        var sb = new StringBuilder($"@hw\tbegin\t{FormVersion}\n");
        int n = 1;
        foreach (string l in body) { sb.Append(l).Append('\n'); n++; }
        string s = sb.ToString();
        return s + $"@hw\tend\t{n}\t{Crc32(Encoding.ASCII.GetBytes(s)):x8}\n";
    }
}
