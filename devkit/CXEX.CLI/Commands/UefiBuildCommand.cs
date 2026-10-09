// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Tools;
using CXEX.CLI.Infrastructure;
using Spectre.Console;
using Spectre.Console.Cli;
using System.ComponentModel;
using System.Threading;

namespace CXEX.CLI.Commands;

/// <summary>
/// Builds the UEFI boot stub with clang and lld-link (HARDENING_PLAN D2, D5).
///
/// Replaces boot/uefi/build.bat, which needed MSVC and an x64 Native Tools
/// prompt - the last part of the repository that did. The toolchain here is
/// the same LLVM the rest of the build uses, so this works on every host.
///
/// The target is deliberately unlike the kernel's: a UEFI application is
/// 64-bit PE32+, while the kernel is 32-bit ELF. Different targets, separate
/// artifacts, and clang --target=i686-elf cannot build this.
/// </summary>
public class UefiBuildCommand : Command<UefiBuildCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandOption("--src <DIR>")]
        [Description("Directory holding cxboot.c (default: boot/uefi next to the repo root)")]
        public string? SourceDir { get; set; }

        [CommandOption("--root <DIR>")]
        [Description("Repository root (default: discovered from the working directory)")]
        public string? Root { get; set; }

        [CommandOption("-o|--out <FILE>")]
        [Description("Output path for the EFI application (default: <src>/BOOTX64.EFI)")]
        public string? Output { get; set; }

        [CommandOption("--cc <EXE>")]
        [Description("C compiler (default: clang)")]
        [DefaultValue("clang")]
        public string Cc { get; set; } = "clang";

        [CommandOption("--linker <EXE>")]
        [Description("Linker (default: lld-link)")]
        [DefaultValue("lld-link")]
        public string Linker { get; set; } = "lld-link";

        [CommandOption("--dry-run")]
        [Description("Print the compile and link command lines and exit")]
        public bool DryRun { get; set; }
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken ct)
    {
        string root = ResolveRoot(s.Root);
        string srcDir = s.SourceDir ?? Path.Combine(root, "boot", "uefi");
        string source = Path.Combine(srcDir, "cxboot.c");
        string abiDir = Path.Combine(root, "abi");

        if (!File.Exists(source))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] no cxboot.c at '{Markup.Escape(source)}'. Pass --src.");
            return 1;
        }
        if (!Directory.Exists(abiDir))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] no abi/ directory at '{Markup.Escape(abiDir)}'. Pass --root.");
            return 1;
        }

        string output = s.Output ?? Path.Combine(srcDir, "BOOTX64.EFI");
        string objPath = Path.Combine(srcDir, "cxboot.obj");

        /* -mno-red-zone is required, not a preference: firmware delivers
           interrupts on the stack this stub is running on, and the red zone
           would be clobbered. -fshort-wchar makes L"..." UTF-16, which is what
           every UEFI string is. Both are documented in boot/uefi/README.md. */
        // Argument lists, not command strings: the runtime quotes each one, so
        // a repository path containing a space no longer depends on the
        // escaping being written out correctly here (security §9).
        string[] compileArgs =
        {
            "-target", "x86_64-unknown-windows", "-ffreestanding", "-fshort-wchar",
            "-mno-red-zone", "-Wall", "-Wextra", $"-I{abiDir}",
            "-c", source, "-o", objPath,
        };

        string[] linkArgs =
        {
            "-subsystem:efi_application", "-entry:efi_main", "-nodefaultlib",
            $"-out:{output}", objPath,
        };

        if (s.DryRun)
        {
            AnsiConsole.WriteLine(ToolProcess.Quote(s.Cc, compileArgs));
            AnsiConsole.WriteLine(ToolProcess.Quote(s.Linker, linkArgs));
            return 0;
        }

        // Named up front rather than failing inside the compiler, the same way
        // `cxk os build` pre-flights its toolchain. ExecutableResolver is no
        // use for this: it returns the name unchanged when it finds nothing,
        // so it can never report absence. ToolProbe actually runs the tool.
        foreach (string tool in new[] { s.Cc, s.Linker })
        {
            string? version = ToolProbe.FirstLine(tool, "--version");
            if (version is null)
            {
                AnsiConsole.MarkupLine($"[red]Error:[/] '{Markup.Escape(tool)}' is not on PATH.");
                AnsiConsole.MarkupLine("[grey]The UEFI stub builds with LLVM on every host; no MSVC is needed.[/]");
                return 1;
            }
            AnsiConsole.MarkupLine($"  [green]ok[/] {Markup.Escape(tool),-9} [grey]{Markup.Escape(version)}[/]");
        }

        AnsiConsole.MarkupLine($"[grey][[1/2]][/] compiling cxboot.c with {Markup.Escape(s.Cc)} ...");
        var cres = ToolProcess.Run(s.Cc, compileArgs, CliTools.Options(workingDirectory: srcDir));
        int rc = cres.Ok ? 0 : 1;
        if (!cres.Ok) CliTools.Report(cres);
        if (rc != 0)
        {
            AnsiConsole.MarkupLine("[red]Error:[/] compile failed.");
            return rc;
        }

        AnsiConsole.MarkupLine($"[grey][[2/2]][/] linking {Markup.Escape(Path.GetFileName(output))} with {Markup.Escape(s.Linker)} ...");
        var lres = ToolProcess.Run(s.Linker, linkArgs, CliTools.Options(workingDirectory: srcDir));
        rc = lres.Ok ? 0 : 1;
        if (!lres.Ok) CliTools.Report(lres);
        if (rc != 0)
        {
            AnsiConsole.MarkupLine("[red]Error:[/] link failed.");
            return rc;
        }

        // The object file is an intermediate; leaving it beside the source
        // puts untracked build output in a source directory.
        try { File.Delete(objPath); } catch (IOException) { /* harmless */ }

        var info = new FileInfo(output);
        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] {Markup.Escape(output)} ({info.Length} bytes)");
        AnsiConsole.MarkupLine("[grey]Copy to an ESP as \\EFI\\BOOT\\BOOTX64.EFI.[/]");
        return 0;
    }

    /// <summary>
    /// Walks up from the working directory looking for the markers that mark a
    /// CXOS checkout, so the command works from anywhere inside one.
    /// </summary>
    private static string ResolveRoot(string? explicitRoot)
    {
        if (!string.IsNullOrEmpty(explicitRoot))
            return Path.GetFullPath(explicitRoot);

        var dir = new DirectoryInfo(Environment.CurrentDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "versions.json")) &&
                Directory.Exists(Path.Combine(dir.FullName, "abi")))
                return dir.FullName;
            dir = dir.Parent;
        }
        return Environment.CurrentDirectory;
    }
}
