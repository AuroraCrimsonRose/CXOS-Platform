// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.CLI.Infrastructure;
using CXEX.Tools;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// Boots a CXK image in an emulator (HARDENING_PLAN D2). The machine options
/// here replace tools/run_qemu_ahci.bat, which existed only because the
/// defaults were not reachable from the CLI.
/// </summary>
public class RunCommand : Command<RunCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<IMAGE_PATH>")]
        [Description("Path to the bootable CXK disk image")]
        public string ImagePath { get; set; } = string.Empty;

        [CommandOption("-e|--emu")]
        [Description("Emulator: qemu (the only one supported; Bochs support was removed)")]
        [DefaultValue("qemu")]
        public string Emulator { get; set; } = "qemu";

        [CommandOption("-M|--machine")]
        [Description("QEMU machine: q35 (AHCI) or pc (i440FX, legacy IDE). " +
                     "Use pc for the ATA self-test, which is vacuous on q35")]
        [DefaultValue("q35")]
        public string Machine { get; set; } = "q35";

        [CommandOption("-m|--memory")]
        [Description("Guest RAM in MiB")]
        [DefaultValue(4096)]
        public int MemoryMb { get; set; } = 4096;

        [CommandOption("--fs-disk")]
        [Description("Second IDE disk image (pc machine only)")]
        public string? FsDisk { get; set; }

        [CommandOption("--snapshot")]
        [Description("Discard all guest writes. CXOS writes to its disk on first boot, " +
                     "so use this whenever the image must survive the run unchanged - " +
                     "verifying a release image without it has already broken its SHA256SUMS")]
        public bool Snapshot { get; set; }

        [CommandOption("--serial")]
        [Description("Where COM1 goes: 'stdio', a file path, or 'none'. The kernel tees " +
                     "its console here when built with CXK_ENABLE_SERIAL, so this is how " +
                     "a boot is captured as text")]
        public string? Serial { get; set; }

        [CommandOption("--headless")]
        [Description("No emulator window (-display none). Implies no audio")]
        public bool Headless { get; set; }

        [CommandOption("--no-audio")]
        [Description("Leave the PC speaker and audio backend out")]
        public bool NoAudio { get; set; }

        [CommandOption("--no-net")]
        [Description("Leave the e1000 NIC out")]
        public bool NoNet { get; set; }

        [CommandOption("--net-trace")]
        [Description("Write a pcap of all NIC traffic to this path")]
        public string? NetTrace { get; set; }

        [CommandOption("--icount")]
        [Description("Deterministic timing: -icount shift=N. 5..7 slows execution enough " +
                     "that boot output can be sampled without scrolling past")]
        public string? ICount { get; set; }

        [CommandOption("--dry-run")]
        [Description("Print the emulator command line and exit")]
        public bool DryRun { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        // Checked before launching, for every emulator. The previous version
        // only reached this test on an unsupported emulator name, so a missing
        // image surfaced as whatever the emulator chose to say about it.
        if (!File.Exists(settings.ImagePath))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] disk image not found: '{Markup.Escape(settings.ImagePath)}'");
            return 1;
        }

        switch (settings.Emulator.ToLowerInvariant())
        {
            case "qemu":
                return RunQemu(settings);

            case "bochs":
                // Named rather than reported as unknown, so an old script or
                // habit gets an answer instead of looking like a typo.
                AnsiConsole.MarkupLine("[red]error:[/] Bochs support was removed.");
                AnsiConsole.MarkupLine("[grey]  QEMU is the only emulator CXOS targets. Every option the Bochs path had[/]");
                AnsiConsole.MarkupLine("[grey]  - memory, boot disk - has a 'cxk run' equivalent; see 'cxk run --help'.[/]");
                return 1;

            default:
                AnsiConsole.MarkupLine(
                    $"[red]Error:[/] unsupported emulator '{Markup.Escape(settings.Emulator)}'. Use 'qemu'.");
                return 1;
        }
    }

    private static int RunQemu(Settings settings)
    {
        string machine = settings.Machine.ToLowerInvariant();
        if (machine != "q35" && machine != "pc")
        {
            AnsiConsole.MarkupLine(
                $"[red]Error:[/] unknown machine '{Markup.Escape(settings.Machine)}'. Use 'q35' or 'pc'.");
            return 1;
        }

        if (settings.FsDisk is not null && machine != "pc")
            AnsiConsole.MarkupLine("[yellow]Note:[/] --fs-disk is only wired up for the pc machine; ignoring it.");

        var config = new QemuConfig
        {
            BootDisk = settings.ImagePath,
            FsDisk = machine == "pc" ? settings.FsDisk : null,
            MachineType = machine,
            MemoryMb = settings.MemoryMb,
            // A headless run has nowhere to put audio, and the dsound backend
            // is not there to be opened on a non-Windows host either.
            EnableAudio = !settings.NoAudio && !settings.Headless,
            EnableNetworking = !settings.NoNet,
            NetTracePath = settings.NetTrace,
            Snapshot = settings.Snapshot,
            Serial = settings.Serial,
            Headless = settings.Headless,
            ICountShift = settings.ICount,
        };

        if (settings.DryRun)
        {
            AnsiConsole.WriteLine(QemuTool.CommandLine(config));
            return 0;
        }

        var result = QemuTool.Run(config, CliTools.Options($"Launching QEMU ({config.MachineType}) with {config.MemoryMb}MB RAM..."));
        return CliTools.Report(result) ? 0 : 1;
    }
}
