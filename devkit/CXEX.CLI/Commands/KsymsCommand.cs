// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.ComponentModel;
using System.Text;
using CXEX.Build.Parsers;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// `cxk ksyms` - the kernel's function symbols, as C source for its second link
/// (kernel/diag/ksym.h), so a panic can name the functions in its trace on a
/// machine with nothing else to read it with.
/// </summary>
public class KsymsCommand : Command<KsymsCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<ELF>")]
        [Description("The kernel's first link, with its symbol table")]
        public string Elf { get; set; } = string.Empty;

        [CommandOption("-o|--output")]
        [Description("The C file to write")]
        public string Output { get; set; } = string.Empty;
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (string.IsNullOrEmpty(settings.Output))
        {
            AnsiConsole.MarkupLine("[red]error:[/] -o is required");
            return 1;
        }
        IReadOnlyList<KernelSymbol> symbols;
        try
        {
            symbols = KernelSymbols.Read(File.ReadAllBytes(settings.Elf));
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {Markup.Escape(settings.Elf)}: {Markup.Escape(e.Message)}");
            return 1;
        }
        File.WriteAllText(settings.Output, KernelSymbols.ToC(symbols), new UTF8Encoding(false));
        return 0;
    }
}
