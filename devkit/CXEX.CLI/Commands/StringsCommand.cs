// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading;
using CXEX.Build.Locale;
using CXEX.Build.Unicode;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// `cxk strings build` - an app's Fluent sources (the toolkit's and its own)
/// to one XSTR string table for one locale (system/CX_TOOLKIT.md §10).
/// </summary>
public class StringsBuildCommand : Command<StringsBuildCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<SOURCES>")]
        [Description("The .ftl sources of the table, all at once, so a key is unique across them")]
        public string[] Sources { get; set; } = Array.Empty<string>();

        [CommandOption("--locale")]
        [Description("The table's locale tag (en, fr, ar...); its plural rules come from CLDR")]
        public string Locale { get; set; } = "en";

        [CommandOption("--base")]
        [Description("The English sources a translation is held to (repeat for each); a key or variable the English lacks is an error")]
        public string[]? Base { get; set; }

        [CommandOption("--pseudo")]
        [Description("Write the pseudo-locale qps from the sources: accented, 40% longer, in brackets, right-to-left")]
        public bool PseudoLocale { get; set; }

        [CommandOption("--rtl")]
        [Description("The locale is written right to left")]
        public bool Rtl { get; set; }

        [CommandOption("--plurals")]
        [Description("CLDR's plurals.xml. Default: vendor/cldr/plurals.xml from the current directory")]
        public string Plurals { get; set; } = Path.Combine("vendor", "cldr", "plurals.xml");

        [CommandOption("-o|--out")]
        [Description("The .xstr to write")]
        public string? Out { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (settings.Sources.Length == 0 || settings.Out is null)
        {
            AnsiConsole.MarkupLine("[red]error:[/] give the sources and -o <table.xstr>");
            return 1;
        }
        try
        {
            foreach (string s in settings.Sources.Concat(settings.Base ?? Array.Empty<string>()))
                if (!File.Exists(s)) throw new FileNotFoundException($"no such file: {s}");
            var cldr = CXEX.Build.Locale.Plurals.Load(settings.Plurals);
            string tag = settings.PseudoLocale ? Pseudo.Tag : settings.Locale;
            var r = Xstr.Build(settings.Sources, tag, cldr, settings.Base, settings.PseudoLocale, settings.Rtl);
            foreach (string w in r.Warnings) AnsiConsole.MarkupLine($"[yellow]warning:[/] {Markup.Escape(w)}");
            File.WriteAllBytes(settings.Out, r.Bytes);
            AnsiConsole.MarkupLine($"[green]{Markup.Escape(tag)}[/]  {r.Messages} messages, {r.Bytes.Length} bytes -> [cyan]{Markup.Escape(settings.Out)}[/]");
            return 0;
        }
        catch (Exception ex) when (ex is InvalidDataException or Fluent.FluentException or IOException or FormatException)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }
    }
}

/// <summary>`cxk strings plurals` - every CLDR locale's rules and samples, for the boot test.</summary>
public class StringsPluralsCommand : Command<StringsPluralsCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<PLURALS_XML>")]
        [Description("CLDR's plurals.xml")]
        public string Path { get; set; } = string.Empty;

        [CommandOption("-o|--out")]
        [Description("The .xplt to write")]
        public string? Out { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (settings.Out is null) { AnsiConsole.MarkupLine("[red]error:[/] -o <file.xplt>"); return 1; }
        try
        {
            var all = CXEX.Build.Locale.Plurals.Load(settings.Path);
            byte[] b = CXEX.Build.Locale.Plurals.Xplt(all);
            File.WriteAllBytes(settings.Out, b);
            AnsiConsole.MarkupLine($"[green]plurals[/]  {all.Sum(a => a.Tags.Length)} locales, {b.Length} bytes -> [cyan]{Markup.Escape(settings.Out)}[/]");
            return 0;
        }
        catch (Exception ex) when (ex is InvalidDataException or IOException or FormatException or System.Xml.XmlException)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }
    }
}

/// <summary>`cxk unicode bidi` - the UAX #9 tables, as os/std/unibidi.xfxn.</summary>
public class UnicodeBidiCommand : Command<UnicodeBidiCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<UCD_DIR>")]
        [Description("A directory with DerivedBidiClass.txt, BidiBrackets.txt and BidiMirroring.txt (vendor/unicode)")]
        public string Dir { get; set; } = string.Empty;

        [CommandOption("-o|--out")]
        [Description("The .xfxn to write (os/std/unibidi.xfxn)")]
        public string? Out { get; set; }

        [CommandOption("--check")]
        [Description("Write nothing: exit 1 if --out differs from what the files give")]
        public bool Check { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (settings.Out is null) { AnsiConsole.MarkupLine("[red]error:[/] -o <unibidi.xfxn>"); return 1; }
        try
        {
            var t = BidiTables.Load(settings.Dir);
            string x = BidiTables.EmitX(t);
            if (settings.Check)
            {
                bool same = File.Exists(settings.Out) && File.ReadAllText(settings.Out) == x;
                AnsiConsole.MarkupLine(same ? "[green]up to date[/]" : $"[red]stale:[/] {Markup.Escape(settings.Out)} is not what Unicode {t.Version}'s files give");
                return same ? 0 : 1;
            }
            File.WriteAllText(settings.Out, x);
            AnsiConsole.MarkupLine($"[green]Unicode {t.Version}[/]  {t.Runs.Count} runs, {t.Brackets.Count} brackets, {t.Mirrors.Count} mirrors -> [cyan]{Markup.Escape(settings.Out)}[/]");
            return 0;
        }
        catch (Exception ex) when (ex is InvalidDataException or IOException or FormatException)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(ex.Message)}");
            return 1;
        }
    }
}
