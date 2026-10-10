// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text.Json;
using CXEX.Core.Constants;
using CXEX.Tests;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// One place decides what the DevKit can read, write and target
/// (DevKit engineering review §13), and it must agree with the registry that
/// decides what the current version of each format IS.
///
/// <para>Two files, two jobs. <c>versions.json</c> is the authority on the
/// current version of each format, and <c>cxk check-versions</c> enforces that
/// every file naming a version agrees with it. What it cannot express is the
/// question a reader asks - <i>can I handle this?</i> - which is a range, not
/// a number, because a reader may legitimately be older than the file it is
/// handed. <see cref="FormatPolicy"/> answers that, and these tests are what
/// stop the two drifting: a format bumped in the registry and forgotten in the
/// policy would otherwise mean the DevKit writing a version it declares it
/// cannot read.</para>
/// </summary>
public class FormatPolicyTests
{
    private static JsonElement? Formats()
    {
        string? repo = TestEnv.RepoRoot;
        if (repo is null) return null;

        string path = Path.Combine(repo, "versions.json");
        if (!File.Exists(path)) return null;

        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        if (!doc.RootElement.TryGetProperty("formats", out var formats)) return null;
        return formats.Clone();
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_version_the_devkit_writes_is_the_registrys_current_version()
    {
        var formats = Formats();
        if (formats is null) { Assert.True(true, "no checkout: skipped"); return; }

        int compared = 0;
        var mismatches = new List<string>();

        foreach (CXFormat format in FormatPolicy.All)
        {
            string key = format.ToString().ToLowerInvariant();
            if (!formats.Value.TryGetProperty(key, out var entry)) continue;
            if (!entry.TryGetProperty("version", out var v)) continue;

            int registry = v.GetInt32();
            int writes = FormatPolicy.WriteVersion(format);
            compared++;

            if (registry != writes)
                mismatches.Add($"{key}: registry {registry}, policy writes {writes}");
        }

        // A comparison that compared nothing would pass silently - the exact
        // failure mode AbiSyncTests was written to rule out for the prelude.
        Assert.True(compared >= 5, $"only {compared} formats matched by name: the comparison is not comparing");
        Assert.Empty(mismatches);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_format_can_read_what_it_writes()
    {
        // The invariant that makes the policy coherent. Stated as a test
        // because a bump that moved Writes without moving Max would leave the
        // DevKit producing artifacts it refuses to load - and the signed path
        // in this repository went unread for long enough once already.
        foreach (CXFormat format in FormatPolicy.All)
        {
            int writes = FormatPolicy.WriteVersion(format);
            Assert.True(FormatPolicy.CanRead(format, writes),
                        $"{format} writes version {writes} but cannot read it");
        }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_range_is_well_formed_and_the_write_version_sits_inside_it()
    {
        foreach (CXFormat format in FormatPolicy.All)
        {
            var s = FormatPolicy.Of(format);
            Assert.True(s.Min >= 1, $"{format}: Min {s.Min} is not a real format version");
            Assert.True(s.Max >= s.Min, $"{format}: Max {s.Max} below Min {s.Min}");
            Assert.InRange(s.Writes, s.Min, s.Max);
        }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Versions_outside_the_range_are_refused_and_the_refusal_says_what_would_pass()
    {
        foreach (CXFormat format in FormatPolicy.All)
        {
            var s = FormatPolicy.Of(format);

            Assert.False(FormatPolicy.CanRead(format, s.Min - 1));
            Assert.False(FormatPolicy.CanRead(format, s.Max + 1));
            Assert.False(FormatPolicy.CanRead(format, 0));

            // A refusal that does not name the acceptable version sends the
            // reader to the source to find out.
            string why = FormatPolicy.ExplainRead(format, s.Max + 1);
            Assert.Contains((s.Max + 1).ToString(), why);
            Assert.Contains(s.Min.ToString(), why);
            Assert.Contains("not supported", why);
        }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Writing_is_a_single_version_not_a_range()
    {
        // Deliberate: a writer that can emit several versions of a format is a
        // compatibility burden nothing has asked for. If that ever changes it
        // should be a decision, which means this test should fail first.
        foreach (CXFormat format in FormatPolicy.All)
        {
            var s = FormatPolicy.Of(format);
            Assert.True(FormatPolicy.CanWrite(format, s.Writes));
            Assert.False(FormatPolicy.CanWrite(format, s.Writes + 1));
            Assert.False(FormatPolicy.CanWrite(format, s.Writes - 1));
        }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_one_constant_that_cannot_read_the_policy_agrees_with_it()
    {
        // CXKeyGenerator.XKPK_VERSION has to be a const: it is a default
        // parameter value, which C# requires to be a compile-time constant.
        // That makes it the one place a format version is still written out
        // independently, so it is compared here rather than trusted.
        Assert.Equal(FormatPolicy.WriteVersion(CXFormat.Xkpk),
                     CXEX.Crypto.Trust.CXKeyGenerator.XKPK_VERSION);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_abi_question_is_answered_from_the_same_table()
    {
        int abi = FormatPolicy.WriteVersion(CXFormat.Abi);
        Assert.True(FormatPolicy.AbiCompatible(abi));
        Assert.False(FormatPolicy.AbiCompatible(abi + 1));
        Assert.False(FormatPolicy.AbiCompatible(abi - 1));
    }
}
