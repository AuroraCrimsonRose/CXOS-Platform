// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// <c>verify-boot</c> on a second firmware: boots a VHDX copy of the image in a
/// Hyper-V Generation 1 VM (AMI BIOS, IDE) and reports PASS, FAIL or
/// INCONCLUSIVE from COM1 as exit codes 0, 1 and 2 (<see cref="HyperVBoot"/>).
/// </summary>
public class VerifyHyperVCommand : Command<VerifyHyperVCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<IMAGE_PATH>")]
        [Description("The disk image to boot. A VHDX copy boots; the image is never modified")]
        public string ImagePath { get; set; } = string.Empty;

        [CommandOption("--full")]
        [Description("Also require the executive's banner and the shell's prompt, as verify-boot --full does")]
        public bool Full { get; set; }

        [CommandOption("-t|--timeout")]
        [Description("Seconds to wait for a verdict (default 300)")]
        [DefaultValue(300)]
        public int TimeoutSeconds { get; set; } = 300;

        [CommandOption("--log")]
        [Description("Where to write the serial log. Default: <image>.hyperv.log")]
        public string? LogPath { get; set; }

        [CommandOption("--memory-mb")]
        [Description("The VM's memory in MB (default 2048)")]
        [DefaultValue(2048)]
        public int MemoryMb { get; set; } = 2048;

        [CommandOption("--cpus")]
        [Description("The VM's processor count (default 1)")]
        [DefaultValue(1)]
        public int Cpus { get; set; } = 1;

        [CommandOption("--keep")]
        [Description("Leave the VM and its VHDX in place afterwards, for a look in Hyper-V Manager")]
        public bool Keep { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.ImagePath))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] disk image not found: '{Markup.Escape(settings.ImagePath)}'");
            return 1;
        }
        if (settings.MemoryMb is < 256 or > 65536 || settings.Cpus is < 1 or > 64)
        {
            AnsiConsole.MarkupLine("[red]error:[/] --memory-mb takes 256 to 65536, --cpus 1 to 64");
            return 1;
        }

        string image = Path.GetFullPath(settings.ImagePath);
        string log = Path.GetFullPath(settings.LogPath is { Length: > 0 } l ? l : settings.ImagePath + ".hyperv.log");
        AnsiConsole.MarkupLine($"[grey]booting[/] {Markup.Escape(image)} [grey]in Hyper-V (Gen 1), serial -> {Markup.Escape(log)}[/]");

        var v = HyperVBoot.Run(image, log, TimeSpan.FromSeconds(settings.TimeoutSeconds), settings.Full,
                               new HyperVBoot.Options(settings.MemoryMb, settings.Cpus, settings.Keep),
                               line => AnsiConsole.MarkupLine($"[grey]{Markup.Escape(line)}[/]"));

        switch (v.Outcome)
        {
            case BootOutcome.Pass:
                AnsiConsole.MarkupLine($"[green]VERDICT: PASS[/] - {Markup.Escape(v.Reason)}");
                return 0;
            case BootOutcome.Fail:
                AnsiConsole.MarkupLine($"[red]VERDICT: FAIL[/] - {Markup.Escape(v.Reason)}");
                AnsiConsole.MarkupLine($"[grey]  full log: {Markup.Escape(log)}[/]");
                return 1;
            default:
                AnsiConsole.MarkupLine($"[yellow]VERDICT: INCONCLUSIVE[/] - {Markup.Escape(v.Reason)}");
                AnsiConsole.MarkupLine($"[grey]  full log: {Markup.Escape(log)}[/]");
                return 2;
        }
    }
}
