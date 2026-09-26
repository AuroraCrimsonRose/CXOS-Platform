using Spectre.Console;

namespace CXEX.CLI.Commands;

/// <summary>
/// Locates a QEMU binary and the OVMF firmware images, across the places the
/// Windows installer and the various Linux distributions put them.
/// </summary>
/// <remarks>
/// Names are searched rather than assumed because there is no single convention:
/// Debian and Ubuntu ship <c>OVMF_CODE_4M.secboot.fd</c> under /usr/share/OVMF,
/// Fedora uses /usr/share/edk2/ovmf, and QEMU's own builds - including the
/// Windows installer - use the <c>edk2-x86_64-*</c> names from its pc-bios
/// directory.
/// </remarks>
public static class UefiFirmware
{
    // Secure-Boot-capable CODE images, best first. The plain OVMF_CODE is NOT
    // listed: it boots unsigned binaries, so finding it automatically would turn
    // a Secure Boot test into a test of nothing.
    private static readonly string[] CodeNames =
    [
        "OVMF_CODE_4M.secboot.fd",
        "OVMF_CODE.secboot.fd",
        "edk2-x86_64-secure-code.fd",
        "OVMF_CODE_4M.ms.fd",
    ];

    private static readonly string[] VarsNames =
    [
        "OVMF_VARS_4M.fd",
        "OVMF_VARS.fd",
        "edk2-i386-vars.fd",
    ];

    private static IEnumerable<string> SearchDirs(string? explicitDir)
    {
        if (!string.IsNullOrEmpty(explicitDir)) yield return explicitDir;

        if (OperatingSystem.IsWindows())
        {
            foreach (var root in new[]
            {
                Environment.GetEnvironmentVariable("ProgramFiles"),
                Environment.GetEnvironmentVariable("ProgramFiles(x86)"),
                @"C:\Program Files", @"C:\Program Files (x86)",
            })
            {
                if (string.IsNullOrEmpty(root)) continue;
                yield return Path.Combine(root, "qemu", "share");
                yield return Path.Combine(root, "qemu");
            }
            // QEMU keeps its firmware next to the executable in its own share dir
            string? exe = FindQemu();
            if (exe is not null)
            {
                string? dir = Path.GetDirectoryName(exe);
                if (dir is not null)
                {
                    yield return Path.Combine(dir, "share");
                    yield return dir;
                }
            }
        }
        else
        {
            yield return "/usr/share/OVMF";
            yield return "/usr/share/ovmf";
            yield return "/usr/share/edk2/ovmf";
            yield return "/usr/share/edk2/x64";
            yield return "/usr/share/qemu";
            yield return "/usr/share/qemu-efi-x86_64";
        }
    }

    public static string? FindCode(string? dir) => Find(dir, CodeNames);
    public static string? FindVarsTemplate(string? dir) => Find(dir, VarsNames);

    private static string? Find(string? dir, string[] names)
    {
        foreach (var d in SearchDirs(dir))
        {
            if (!Directory.Exists(d)) continue;
            foreach (var n in names)
            {
                string p = Path.Combine(d, n);
                if (File.Exists(p)) return p;
            }
        }
        return null;
    }

    /// <summary>Locates qemu-system-x86_64, on PATH or in the usual install roots.</summary>
    public static string? FindQemu()
    {
        string exe = OperatingSystem.IsWindows() ? "qemu-system-x86_64.exe" : "qemu-system-x86_64";

        string? path = Environment.GetEnvironmentVariable("PATH");
        foreach (var d in (path ?? "").Split(Path.PathSeparator))
        {
            if (string.IsNullOrWhiteSpace(d)) continue;
            try { if (File.Exists(Path.Combine(d, exe))) return Path.Combine(d, exe); }
            catch (ArgumentException) { /* a malformed PATH entry is not fatal */ }
        }

        if (OperatingSystem.IsWindows())
            foreach (var root in new[]
            {
                Environment.GetEnvironmentVariable("ProgramFiles"),
                @"C:\Program Files", @"C:\qemu",
            })
            {
                if (string.IsNullOrEmpty(root)) continue;
                string p = Path.Combine(root, "qemu", exe);
                if (File.Exists(p)) return p;
                p = Path.Combine(root, exe);
                if (File.Exists(p)) return p;
            }
        return null;
    }

    /// <summary>Prints where we looked, so a missing file is actionable.</summary>
    public static void ReportSearch(string? dir)
    {
        AnsiConsole.MarkupLine("  Looked in:");
        foreach (var d in SearchDirs(dir).Distinct())
            AnsiConsole.MarkupLine($"    [grey]{d}[/]{(Directory.Exists(d) ? "" : " (does not exist)")}");
        AnsiConsole.MarkupLine("  For a Secure-Boot-capable CODE image named one of:");
        foreach (var n in CodeNames) AnsiConsole.MarkupLine($"    [grey]{n}[/]");
        AnsiConsole.MarkupLine("  and a vars template named one of:");
        foreach (var n in VarsNames) AnsiConsole.MarkupLine($"    [grey]{n}[/]");
        AnsiConsole.MarkupLine("");
        AnsiConsole.MarkupLine("  On Windows these ship with the QEMU installer, under its [grey]share\\[/] directory.");
        AnsiConsole.MarkupLine("  Pass [grey]--firmware-dir[/] if yours are somewhere else.");
    }
}
