// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text.Json;
using System.Text.RegularExpressions;

namespace CXEX.Tools;

/// <summary>One external tool the build runs, as versions.json's <c>toolchain</c> group declares it.</summary>
/// <param name="Minimum">The oldest version known to work, or null when no floor is known.</param>
public sealed record ToolPin(string Name, string[] Probe, Regex Pattern, Version? Minimum, string Role, string Evidence);

/// <summary>What probing one tool found.</summary>
/// <param name="Banner">The tool's first line of version output, verbatim: its identity. Null if not found.</param>
/// <param name="Version">The version parsed out of the banner, or null if it could not be.</param>
public sealed record ToolFinding(ToolPin Pin, string? Banner, Version? Version)
{
    public bool Found => Banner is not null;

    /// <summary>
    /// Too old, or unreadable when a floor exists. A banner the pattern cannot
    /// read is refused rather than waved through: "could not tell" is not
    /// evidence the tool is new enough, and the clang this floor exists for
    /// fails a hundred steps into the build, nowhere near the cause.
    /// </summary>
    public bool BelowMinimum => Pin.Minimum is not null && Found && (Version is null || Version < Pin.Minimum);
}

/// <summary>
/// The toolchain pins (security review §13, engineering review §9): which tools
/// the build needs, the oldest version of each known to work, and why.
///
/// <para>The pins live in versions.json because that file decides every
/// version. This class only reads them, so a floor is changed there and nowhere
/// else - and the CI image and CMake are tied to the same entries by tests.</para>
/// </summary>
public static class ToolchainPolicy
{
    public static IReadOnlyList<ToolPin> Load(string versionsJsonPath)
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(versionsJsonPath));
        if (!doc.RootElement.TryGetProperty("toolchain", out var group))
            throw new InvalidDataException($"{versionsJsonPath} has no \"toolchain\" group.");

        var pins = new List<ToolPin>();
        foreach (var tool in group.EnumerateObject())
        {
            var t = tool.Value;
            string Req(string field) =>
                t.TryGetProperty(field, out var v) && v.ValueKind == JsonValueKind.String && v.GetString() is { Length: > 0 } s
                    ? s
                    : throw new InvalidDataException($"toolchain.{tool.Name}.{field} is missing or empty.");

            Version? minimum = null;
            if (t.TryGetProperty("minimum", out var m) && m.ValueKind != JsonValueKind.Null)
            {
                minimum = ParseVersion(m.GetString())
                          ?? throw new InvalidDataException($"toolchain.{tool.Name}.minimum \"{m}\" is not a version.");
            }

            if (!t.TryGetProperty("probe", out var probe) || probe.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException($"toolchain.{tool.Name}.probe must be an array of arguments.");

            pins.Add(new ToolPin(
                tool.Name,
                probe.EnumerateArray().Select(a => a.GetString() ?? "").ToArray(),
                new Regex(Req("pattern"), RegexOptions.CultureInvariant),
                minimum,
                Req("role"),
                Req("evidence")));
        }
        return pins;
    }

    /// <summary>Reads a version out of a banner with the pin's pattern; null if it does not match.</summary>
    public static Version? ReadVersion(ToolPin pin, string banner)
    {
        var m = pin.Pattern.Match(banner);
        return m.Success && m.Groups.Count > 1 ? ParseVersion(m.Groups[1].Value) : null;
    }

    /// <summary>
    /// "23", "3.02" and "23.1.2" alike, always widened to at least three parts.
    ///
    /// <para>The widening is not cosmetic. System.Version treats a missing part
    /// as -1, so <c>23.0 &lt; 23.0.0</c>: left as parsed, a floor of "23" sits
    /// strictly below clang 23.0.0, and a comparison that wrongly refused a
    /// version equal to the floor would never meet a version equal to it.
    /// Found by exactly that sabotage passing.</para>
    /// </summary>
    public static Version? ParseVersion(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        var parts = text.Trim().Split('.').ToList();
        if (parts.Count > 4 || parts.Any(p => p.Length == 0 || !p.All(char.IsAsciiDigit))) return null;
        while (parts.Count < 3) parts.Add("0");
        return Version.TryParse(string.Join('.', parts), out var v) ? v : null;
    }

    /// <summary>Probes every pinned tool. Pure apart from running the tools.</summary>
    public static IReadOnlyList<ToolFinding> Probe(IReadOnlyList<ToolPin> pins) =>
        pins.Select(p =>
        {
            string? banner = ToolProbe.FirstLine(p.Name, p.Probe);
            return new ToolFinding(p, banner, banner is null ? null : ReadVersion(p, banner));
        }).ToList();
}
