// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using CXEX.CLI.Infrastructure;
using CXEX.Tools;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Builds the OS: boot chain, kernel, executive, X userland and the bootable
/// disk image. Replaces tools/build.bat (HARDENING_PLAN D2), and builds through
/// clang, ld.lld and Ninja on every host (D5) - no i686-elf GCC, no NMake, no
/// MSVC Developer Command Prompt.
///
/// The steps are build.bat's, in the same order: toolchain pre-flight, the
/// source-list and ABI pre-flights, then configure and build. What it adds is a
/// toolchain probe that names a missing or too-old tool, instead of failing a hundred
/// lines later inside CMake.
/// </summary>
public class OsBuildCommand : Command<OsBuildCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandOption("--dev")]
        [Description("Development kernel: runs UNSIGNED images, signing skipped. Never ship one.")]
        public bool Dev { get; set; }

        [CommandOption("--key <NAME>")]
        [Description("Signing key pair in tools/, by basename: --key test uses tools/test.xksk + tools/test.xkpk. Default: kernel.")]
        public string? Key { get; set; }

        [CommandOption("--root <DIR>")]
        [Description("Repository root. Default: found by walking up from the working directory.")]
        public string? Root { get; set; }

        [CommandOption("--build-dir <DIR>")]
        [Description("Build directory. Default: <root>/build.")]
        public string? BuildDir { get; set; }

        [CommandOption("--clean")]
        [Description("Delete the build directory first, forcing a full rebuild.")]
        public bool Clean { get; set; }

        [CommandOption("-j|--jobs <N>")]
        [Description("Parallel build jobs. Default: Ninja's own choice.")]
        public int Jobs { get; set; }
    }

    protected override int Execute(CommandContext context, Settings s, CancellationToken ct)
    {
        AnsiConsole.Write(new Rule("[bold]CXK - BUILD[/]").LeftJustified());

        // ---- the repository root ----
        string? root = ResolveRoot(s.Root);
        if (root is null)
        {
            AnsiConsole.MarkupLine("[red]error:[/] could not find the repository root.");
            AnsiConsole.MarkupLine("[grey]  Run this inside the repository, or pass:  cxk os build --root <DIR>[/]");
            return 1;
        }

        string cmakeDir = Path.Combine(root, "tools", "cmake");
        string cmakeList = Path.Combine(cmakeDir, "CMakeLists.txt");
        string abiHeader = Path.Combine(root, "abi", "cxk_abi.h");
        // The key is named, not pathed: both halves must be halves of the same
        // pair, and the kernel's root of trust is generated from the public one,
        // so there is nothing to be gained by letting them be chosen separately.
        string keyName = s.Key is { Length: > 0 } k ? k : "kernel";
        string signingKey = Path.Combine(root, "tools", keyName + ".xksk");
        string publicKey  = Path.Combine(root, "tools", keyName + ".xkpk");
        string buildDir = s.BuildDir is { Length: > 0 } b ? Path.GetFullPath(b) : Path.Combine(root, "build");

        AnsiConsole.MarkupLine($"[grey]root:[/]  {root}");
        AnsiConsole.MarkupLine($"[grey]build:[/] {buildDir}");

        // ---- [1/5] toolchain ----
        // Probed before anything else: a missing tool is the one failure that
        // should name itself, rather than surfacing as a CMake error about a
        // compiler it could not identify.
        AnsiConsole.MarkupLine("");
        AnsiConsole.MarkupLine("[bold][[1/5]][/] Toolchain");
        var toolchain = ProbeToolchain(root);
        if (toolchain is null) return 1;

        // ---- [2/5] source list ----
        AnsiConsole.MarkupLine("");
        AnsiConsole.MarkupLine("[bold][[2/5]][/] Pre-flight: every source the build lists exists");
        if (!File.Exists(cmakeList))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] build not found: {cmakeList}");
            return 1;
        }

        int rc = RunPreflight(new CheckCommand(), context, new CheckCommand.Settings { CmakePath = cmakeList, Root = root }, ct);
        if (rc != 0)
        {
            AnsiConsole.MarkupLine("[red]error:[/] source pre-flight failed.");
            return rc;
        }

        // ---- [3/5] ABI ----
        // The X compiler carries a hand-maintained copy of abi/cxk_abi.h as its
        // prelude. When they drift, the userland fails to compile with
        // "undefined name" a long way from the cause - so catch it here instead.
        AnsiConsole.MarkupLine("");
        AnsiConsole.MarkupLine("[bold][[3/5]][/] Pre-flight: the X ABI prelude matches cxk_abi.h");
        if (!File.Exists(abiHeader))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] ABI header not found: {abiHeader}");
            return 1;
        }

        rc = RunPreflight(new CheckVersionsCommand(), context, new CheckVersionsCommand.Settings { Root = root, Quiet = true }, ct);
        if (rc != 0)
        {
            AnsiConsole.MarkupLine("[red]error:[/] version pre-flight failed.");
            return rc;
        }

        rc = RunPreflight(new CheckAbiCommand(), context, new CheckAbiCommand.Settings { HeaderPath = abiHeader, Quiet = true }, ct);
        if (rc != 0)
        {
            AnsiConsole.MarkupLine("[red]error:[/] ABI pre-flight failed.");
            return rc;
        }

        // ---- signing ----
        // A development kernel runs unsigned images, so a test boot needs no key
        // at all. Signing is off for it even when a key is present: CMake refuses
        // the combination, because a signed kernel that runs unsigned code looks
        // official and trusts nothing.
        var defines = new List<string>();
        bool sign = !s.Dev && File.Exists(signingKey);
        defines.Add(sign ? "-DSIGN=ON" : "-DSIGN=OFF");
        if (s.Dev) defines.Add("-DDEV_UNSIGNED=ON");
        defines.Add($"-DCXK_KEY={keyName}");

        // The public half is needed whether or not anything is signed: it is what
        // the kernel's root of trust is generated from. Named here rather than
        // left to CMake so a typo'd --key says so before configuring.
        if (!File.Exists(publicKey))
        {
            AnsiConsole.MarkupLine($"[red]error:[/] no public key to trust: {publicKey}");
            AnsiConsole.MarkupLine($"[grey]  run:  cxk keygen {Path.Combine(root, "tools", keyName)}[/]");
            return 1;
        }

        // Point CMake at THIS cxk rather than letting it guess. Its fallback is a
        // path ending in .exe, which is simply wrong off Windows - so `cxk os
        // build` could not work on Linux or macOS, which is most of what D5's
        // "one toolchain on every host" is supposed to mean. The running
        // executable is also the right answer: it is the one the user invoked,
        // so the build cannot silently package with a different, older cxk that
        // happens to be sitting in tools/.
        string? self = Environment.ProcessPath;
        if (self is { Length: > 0 })
            defines.Add($"-DCXK={self.Replace('\\', '/')}");

        AnsiConsole.MarkupLine("");
        if (s.Dev)
            AnsiConsole.MarkupLine("[yellow]DEVELOPMENT build[/] - unsigned images will run, signing skipped. Never ship this image.");
        else if (sign)
            AnsiConsole.MarkupLine($"[green]signing key found[/] - artifacts will be SIGNED with tools/{keyName}.xksk");
        else
            AnsiConsole.MarkupLine($"[yellow]no signing key[/] - building UNSIGNED (run: cxk keygen tools/{keyName})");
        AnsiConsole.MarkupLine($"[grey]trusted key:[/] tools/{keyName}.xkpk, compiled into the kernel");

        // ---- [4/5] configure ----
        if (s.Clean && Directory.Exists(buildDir))
        {
            AnsiConsole.MarkupLine($"[grey]cleaning:[/] {buildDir}");
            try
            {
                Directory.Delete(buildDir, recursive: true);
            }
            catch (Exception ex)
            {
                AnsiConsole.MarkupLine($"[red]error:[/] could not delete {buildDir}: {ex.Message}");
                return 1;
            }
        }

        AnsiConsole.MarkupLine("");
        AnsiConsole.MarkupLine($"[bold][[4/5]][/] Configuring CMake ({CMakeTool.Generator}) {string.Join(" ", defines)}");
        if (!CliTools.Report(CMakeTool.Configure(cmakeDir, buildDir, defines, CliTools.Options())))
        {
            AnsiConsole.MarkupLine("[red]error:[/] CMake configure failed.");
            return 1;
        }

        // ---- [5/5] build ----
        AnsiConsole.MarkupLine("");
        AnsiConsole.MarkupLine("[bold][[5/5]][/] Building");
        if (!CliTools.Report(CMakeTool.Build(buildDir, s.Jobs, CliTools.Options())))
        {
            AnsiConsole.MarkupLine("[red]error:[/] build failed.");
            return 1;
        }

        WriteToolchainRecord(root, toolchain, s.Dev ? "development, unsigned" : sign ? $"signed, tools/{keyName}.xksk" : "unsigned");

        string packages = Path.Combine(root, "dist", "CXK_x86_64", "packages");
        string image = Path.Combine(root, "dist", "CXK_x86_64", "images", "cxk_x86_64.img");

        AnsiConsole.Write(new Rule("[bold green]Done[/]").LeftJustified());
        AnsiConsole.MarkupLine($"[grey]kernel:[/] {Path.Combine(buildDir, "kernel.elf")}");
        AnsiConsole.MarkupLine($"[grey]exec:[/]   {Path.Combine(packages, "executive.xoex")}");
        AnsiConsole.MarkupLine($"[grey]disk:[/]   {image}");
        AnsiConsole.MarkupLine($"[grey]run:[/]    cxk run {image}");
        return 0;
    }

    /// <summary>
    /// Runs one of the existing pre-flight commands in process, so the build and
    /// a bare `cxk check` apply exactly the same rules. Their Execute is
    /// protected, as Spectre intends; ICommand is the supported way in, and it
    /// is async-only, so the result is awaited here - these are short, local
    /// checks and the build cannot proceed until they answer.
    /// </summary>
    private static int RunPreflight(ICommand command, CommandContext context, CommandSettings settings, CancellationToken ct) =>
        command.ExecuteAsync(context, settings, ct).GetAwaiter().GetResult();

    /// <summary>
    /// Probes every tool versions.json's <c>toolchain</c> group lists, and refuses
    /// one that is missing or older than its pinned minimum. The list and the
    /// floors both come from that file (security §13, engineering §9), so this
    /// method has no opinion of its own about which clang is new enough.
    ///
    /// <para>The floor exists because of a failure that names the wrong thing:
    /// clang 18-22 build for i686-elf by linking through gcc, and die a hundred
    /// steps in with "gcc doesn't exist". Refused here, it says "clang 22 is
    /// older than 23" instead.</para>
    /// </summary>
    private static IReadOnlyList<ToolFinding>? ProbeToolchain(string root)
    {
        IReadOnlyList<ToolPin> pins;
        try
        {
            pins = ToolchainPolicy.Load(Path.Combine(root, "versions.json"));
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or System.Text.Json.JsonException)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] cannot read the toolchain pins: {Markup.Escape(ex.Message)}");
            return null;
        }

        var findings = ToolchainPolicy.Probe(pins);
        foreach (var f in findings)
        {
            string name = f.Pin.Name;
            if (!f.Found)
                AnsiConsole.MarkupLine($"  [red]x[/]  {name,-7} [red]not found[/]");
            else if (f.BelowMinimum)
                AnsiConsole.MarkupLine($"  [red]x[/]  {name,-7} [red]{Markup.Escape(f.Banner!)}[/]");
            else
                AnsiConsole.MarkupLine($"  [green]ok[/] {name,-7} [grey]{Markup.Escape(f.Banner!)}[/]");
        }

        var missing = findings.Where(f => !f.Found).ToList();
        var old = findings.Where(f => f.BelowMinimum).ToList();
        if (missing.Count == 0 && old.Count == 0) return findings;

        AnsiConsole.MarkupLine("");
        if (missing.Count > 0)
        {
            AnsiConsole.MarkupLine($"[red]error:[/] {missing.Count} required tool(s) missing from PATH:");
            foreach (var f in missing)
                AnsiConsole.MarkupLine($"  [red]{f.Pin.Name}[/] - {Markup.Escape(f.Pin.Role)}");
        }
        foreach (var f in old)
        {
            string found = f.Version is null ? "a version this build cannot read" : f.Version.ToString();
            AnsiConsole.MarkupLine($"[red]error:[/] {f.Pin.Name} is {Markup.Escape(found)}; the build needs {f.Pin.Minimum} or newer.");
            AnsiConsole.MarkupLine($"[grey]  {Markup.Escape(f.Pin.Evidence)}[/]");
        }
        return null;
    }

    /// <summary>
    /// Records what the build ran with, beside what it produced (security §13,
    /// engineering §9). The banners are kept verbatim because they are the
    /// identity: clang's carries the exact LLVM commit.
    ///
    /// <para>Deliberately no timestamp, no machine name and no paths: two builds
    /// with the same tools must write the same record, or dist/ stops being
    /// reproducible (CX_EXTENSION_SYSTEM.md §9.9).</para>
    /// </summary>
    private static void WriteToolchainRecord(string root, IReadOnlyList<ToolFinding> findings, string signing)
    {
        string dir = Path.Combine(root, "dist", "CXK_x86_64");
        Directory.CreateDirectory(dir);

        string cxk = typeof(OsBuildCommand).Assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "unknown";

        using var ms = new MemoryStream();
        using (var w = new System.Text.Json.Utf8JsonWriter(ms, new System.Text.Json.JsonWriterOptions
        {
            Indented = true,
            // Relaxed so the record reads as written: a version's "+commit" would
            // otherwise come out as +. Nothing here is ever embedded in HTML.
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        }))
        {
            w.WriteStartObject();
            w.WriteString("cxk", cxk);
            w.WriteString("host", System.Runtime.InteropServices.RuntimeInformation.RuntimeIdentifier);
            w.WriteString("target", "x86_64-elf");
            w.WriteString("signing", signing);
            w.WriteStartObject("tools");
            foreach (var f in findings)
                w.WriteString(f.Pin.Name, f.Banner);
            w.WriteEndObject();
            w.WriteEndObject();
        }
        ms.WriteByte((byte)'\n');
        File.WriteAllBytes(Path.Combine(dir, "toolchain.json"), ms.ToArray());
    }

    /// <summary>
    /// Finds the repository root by walking up from the working directory looking
    /// for the build and the ABI header together. Both, not either: a lone tools/
    /// directory is common enough to match the wrong tree.
    /// </summary>
    internal static string? ResolveRoot(string? explicitRoot)
    {
        if (explicitRoot is { Length: > 0 })
        {
            string full = Path.GetFullPath(explicitRoot);
            return IsRoot(full) ? full : null;
        }

        var dir = new DirectoryInfo(Environment.CurrentDirectory);
        while (dir is not null)
        {
            if (IsRoot(dir.FullName)) return dir.FullName;
            dir = dir.Parent;
        }

        // Fall back to where cxk itself lives: it normally sits in tools/, so its
        // own location identifies the tree even when the working directory is
        // somewhere else entirely.
        string? exeDir = Path.GetDirectoryName(Environment.ProcessPath);
        var fromExe = exeDir is null ? null : new DirectoryInfo(exeDir);
        while (fromExe is not null)
        {
            if (IsRoot(fromExe.FullName)) return fromExe.FullName;
            fromExe = fromExe.Parent;
        }

        return null;
    }

    private static bool IsRoot(string dir) =>
        File.Exists(Path.Combine(dir, "tools", "cmake", "CMakeLists.txt")) &&
        File.Exists(Path.Combine(dir, "abi", "cxk_abi.h"));
}
