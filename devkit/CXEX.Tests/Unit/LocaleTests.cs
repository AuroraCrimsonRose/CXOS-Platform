// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using CXEX.Build.Locale;
using CXEX.Build.Unicode;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// Localisation's host side (system/CX_TOOLKIT.md §10): the Fluent subset,
/// XSTR tables and their checks, CLDR's plural rules compiled, the
/// pseudo-locale, and the bidi tables os/std/unibidi.xfxn holds. The X side
/// (std/locale.xfxn, std/bidi.xfxn) is held to the same files at boot and in
/// the Differential run.
/// </summary>
public class LocaleTests
{
    private static string Cldr => Path.Combine(Requires.Repo(), "vendor", "cldr", "plurals.xml");

    private static string Ftl(string dir, string name, string text)
    {
        string p = Path.Combine(dir, name);
        File.WriteAllText(p, text);
        return p;
    }

    private static string TempDir()
    {
        string d = Path.Combine(Path.GetTempPath(), "cxk-locale-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(d);
        return d;
    }

    private const string English =
        "# the toolkit\n" +
        "hello = Hello, { $name }!\n" +
        "files = { $n ->\n" +
        "    [one] One file\n" +
        "    [0] No files\n" +
        "   *[other] { $n } files\n" +
        " }\n" +
        "save = &Save\n" +
        "    .tip = Saves the file\n" +
        "brace = A {\"{\"} brace\n";

    // ---- Fluent ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Fluent_reads_messages_attributes_variables_selects_and_literals()
    {
        var m = Fluent.Parse(English, "en.ftl").ToDictionary(x => x.Key);
        Assert.Equal(new[] { "brace", "files", "hello", "save", "save.tip" }, m.Keys.OrderBy(k => k, StringComparer.Ordinal));
        Assert.Equal(new[] { "name" }, Fluent.Variables(m["hello"].Pattern));
        var sel = Fluent.Selects(m["files"].Pattern).Single();
        Assert.Equal("n", sel.Name);
        Assert.Equal(3, sel.Variants.Count);
        Assert.Single(sel.Variants, v => v.IsDefault);
        Assert.Contains(sel.Variants, v => v.IsNumber && v.Number == 0);
        string brace = string.Concat(m["brace"].Pattern.OfType<Fluent.Text>().Select(t => t.Value));
        Assert.Equal("A { brace", brace);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("-term = Brand\n", "term")]
    [InlineData("a = { NUMBER($n) }\n", "function")]
    [InlineData("a = { other-message }\n", "reference")]
    [InlineData("a = { $n ->\n    [one] x\n    [other] y\n }\n", "default")]
    [InlineData("a = { $n ->\n   *[one] x\n   *[other] y\n }\n", "default")]
    [InlineData("a = x\na = y\n", "again")]
    public void Fluent_refuses_what_the_subset_does_not_have(string src, string word)
    {
        var dir = TempDir();
        string p = Ftl(dir, "en.ftl", src);
        var ex = Assert.ThrowsAny<Exception>(() => Xstr.Build(new[] { p }, "en", Plurals.Load(Cldr), null, false, false));
        Assert.Contains(word, ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    // ---- XSTR ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_table_reads_back_as_written_with_its_flags_tag_and_rules()
    {
        var dir = TempDir();
        string en = Ftl(dir, "en.ftl", English);
        var r = Xstr.Build(new[] { en }, "ar", Plurals.Load(Cldr), null, false, rtl: true);
        var back = Xstr.Read(r.Bytes);
        Assert.Equal("ar", back.Tag);
        Assert.True(back.Rtl);
        Assert.Equal(5, back.Bodies.Count);
        Assert.Equal(Plurals.Block(Plurals.For(Plurals.Load(Cldr), "ar")), back.PluralBlock);
        Assert.Equal((uint)Xstr.HeaderSize, BitConverter.ToUInt32(r.Bytes, 28));
        Assert.Equal((uint)r.Bytes.Length, BitConverter.ToUInt32(r.Bytes, 36));
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData(0, 0x00, "magic")]
    [InlineData(4, 0x02, "version")]
    [InlineData(6, 0x02, "flags")]
    [InlineData(8, 0x00, "tag")]
    [InlineData(23, 0x41, "tag")]
    [InlineData(28, 0x00, "index")]
    [InlineData(36, 0x01, "size")]
    [InlineData(32, 0x00, "plural")]
    public void Each_header_field_the_reader_checks_is_refused_when_wrong(int offset, byte value, string word)
    {
        var dir = TempDir();
        var r = Xstr.Build(new[] { Ftl(dir, "en.ftl", English) }, "en", Plurals.Load(Cldr), null, false, false);
        byte[] b = (byte[])r.Bytes.Clone();
        b[offset] = value;
        var ex = Assert.Throws<InvalidDataException>(() => Xstr.Read(b));
        Assert.Contains(word, ex.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Keys_out_of_order_a_bad_op_and_a_truncated_table_are_refused()
    {
        var dir = TempDir();
        byte[] good = Xstr.Build(new[] { Ftl(dir, "en.ftl", English) }, "en", Plurals.Load(Cldr), null, false, false).Bytes;

        byte[] swapped = (byte[])good.Clone();      // records 0 and 1 swapped: keys fall
        Array.Copy(good, Xstr.HeaderSize, swapped, Xstr.HeaderSize + Xstr.RecordSize, Xstr.RecordSize);
        Array.Copy(good, Xstr.HeaderSize + Xstr.RecordSize, swapped, Xstr.HeaderSize, Xstr.RecordSize);
        Assert.Contains("rising", Assert.Throws<InvalidDataException>(() => Xstr.Read(swapped)).Message);

        byte[] badop = (byte[])good.Clone();         // the first body's first op
        badop[(int)BitConverter.ToUInt32(good, Xstr.HeaderSize + 8)] = 9;
        Assert.Contains("op", Assert.Throws<InvalidDataException>(() => Xstr.Read(badop)).Message);

        Assert.Throws<InvalidDataException>(() => Xstr.Read(good[..(good.Length - 1)]));
        Assert.Throws<InvalidDataException>(() => Xstr.Read(good[..20]));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_translation_is_held_to_the_English()
    {
        var dir = TempDir();
        string en = Ftl(dir, "en.ftl", English);
        var cldr = Plurals.Load(Cldr);

        string unknown = Ftl(dir, "fr1.ftl", "bonjour = Bonjour\n");
        Assert.Contains("not in the English", Assert.Throws<InvalidDataException>(
            () => Xstr.Build(new[] { unknown }, "fr", cldr, new[] { en }, false, false)).Message);

        string extraVar = Ftl(dir, "fr2.ftl", "hello = Bonjour, { $nom } !\n");
        Assert.Contains("$nom", Assert.Throws<InvalidDataException>(
            () => Xstr.Build(new[] { extraVar }, "fr", cldr, new[] { en }, false, false)).Message);

        // Japanese has only `other`: a [one] variant is a category it does not have
        string badCat = Ftl(dir, "ja.ftl", "files = { $n ->\n    [one] x\n   *[other] y\n }\n");
        Assert.Contains("no such plural category", Assert.Throws<InvalidDataException>(
            () => Xstr.Build(new[] { badCat }, "ja", cldr, new[] { en }, false, false)).Message);

        // a missing key is a warning, not an error: English shows instead
        string partial = Ftl(dir, "fr3.ftl", "hello = Bonjour, { $name } !\n");
        var ok = Xstr.Build(new[] { partial }, "fr", cldr, new[] { en }, false, false);
        Assert.Contains(ok.Warnings, w => w.Contains("files"));
    }

    // ---- plurals ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void CLDRs_integer_samples_hold_under_the_compiled_rules_for_every_locale()
    {
        var all = Plurals.Load(Cldr);
        Assert.True(all.Sum(a => a.Tags.Length) > 200);   // 228 tags in CLDR 49, in fewer rule groups
        int checkedSamples = 0;
        foreach (var lr in all)
        {
            foreach (var r in lr.Rules)
                foreach (uint v in r.Samples) { Assert.Equal(r.Category, Plurals.Evaluate(lr, v)); checkedSamples++; }
            foreach (uint v in lr.OtherSamples) { Assert.Equal(Plurals.Other, Plurals.Evaluate(lr, v)); checkedSamples++; }
        }
        Assert.True(checkedSamples > 1000);
        // spot values worth knowing by heart
        var en = Plurals.For(all, "en"); var ru = Plurals.For(all, "ru"); var ar = Plurals.For(all, "ar");
        Assert.Equal(1, Plurals.Evaluate(en, 1));
        Assert.Equal(5, Plurals.Evaluate(en, 0));
        Assert.Equal(1, Plurals.Evaluate(ru, 21));       // one
        Assert.Equal(3, Plurals.Evaluate(ru, 22));       // few
        Assert.Equal(4, Plurals.Evaluate(ru, 11));       // many
        Assert.Equal(0, Plurals.Evaluate(ar, 0));        // zero
        Assert.Equal(2, Plurals.Evaluate(ar, 2));        // two
        Assert.Equal(Plurals.For(all, "pt"), Plurals.For(all, "pt-XX"));   // a region CLDR lacks falls to its language
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_xplt_file_carries_a_record_per_tag()
    {
        var all = Plurals.Load(Cldr);
        byte[] x = Plurals.Xplt(all);
        Assert.Equal(Plurals.XpltMagic, BitConverter.ToUInt32(x, 0));
        Assert.Equal(Xstr.Version, BitConverter.ToUInt16(x, 4));
        Assert.Equal((uint)all.Sum(a => a.Tags.Length), BitConverter.ToUInt32(x, 8));
        Assert.Equal((uint)x.Length, BitConverter.ToUInt32(x, 12));
    }

    // ---- pseudo ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_pseudo_locale_accents_lengthens_brackets_keeps_mnemonics_and_is_rtl()
    {
        var dir = TempDir();
        string en = Ftl(dir, "en.ftl", English);
        var r = Xstr.Build(new[] { en }, Pseudo.Tag, Plurals.Load(Cldr), null, pseudo: true, rtl: false);
        var t = Xstr.Read(r.Bytes);
        Assert.Equal("qps", t.Tag);
        Assert.True(t.Rtl);
        string save = Encoding.UTF8.GetString(t.Bodies["save"][3..]);
        Assert.StartsWith("[&S", save);              // the mnemonic's letter stays as it is
        Assert.EndsWith("~]", save);
        Assert.Equal("Héllö", Pseudo.Accent("Hello"));
        Assert.True(save.Length >= (int)Math.Ceiling("&Save".Length * 1.4));
    }

    // ---- the bidi tables ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Unibidi_is_what_the_vendored_UCD_files_give()
    {
        string repo = Requires.Repo();
        var t = BidiTables.Load(Path.Combine(repo, "vendor", "unicode"));
        string want = BidiTables.EmitX(t);
        string have = File.ReadAllText(Path.Combine(repo, "os", "std", "unibidi.xfxn"));
        Assert.True(want == have, "os/std/unibidi.xfxn is stale: run `cxk unicode bidi vendor/unicode -o os/std/unibidi.xfxn`");
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_bidi_tables_know_the_classes_brackets_and_mirrors_that_matter()
    {
        var t = BidiTables.Load(Path.Combine(Requires.Repo(), "vendor", "unicode"));
        int C(string n) => Array.IndexOf(BidiTables.Classes, n);
        Assert.Equal(C("L"), t.ClassOf['a']);
        Assert.Equal(C("R"), t.ClassOf[0x05D0]);        // Hebrew alef
        Assert.Equal(C("AL"), t.ClassOf[0x0627]);       // Arabic alef
        Assert.Equal(C("EN"), t.ClassOf['1']);
        Assert.Equal(C("AN"), t.ClassOf[0x0661]);
        Assert.Equal(C("WS"), t.ClassOf[' ']);
        Assert.Equal(C("BN"), t.ClassOf[0x200B]);
        Assert.Equal(C("FSI"), t.ClassOf[0x2068]);
        Assert.Equal(C("R"), t.ClassOf[0x05FF]);        // unassigned, but @missing says R
        Assert.Contains(t.Brackets, b => b.Cp == '(' && b.Pair == ')' && b.Open);
        Assert.Contains(t.Mirrors, m => m.Cp == '<' && m.To == '>');
        // each run differs from the one before, and they cover the code space from 0
        Assert.Equal(0, t.Runs[0].Start);
        for (int k = 1; k < t.Runs.Count; k++) Assert.NotEqual(t.Runs[k - 1].Class, t.Runs[k].Class);
    }
}
