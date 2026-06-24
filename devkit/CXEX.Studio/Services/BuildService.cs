using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using CXEX.Build.Emitters;
using CXEX.Build.Engines;
using CXEX.Build.Parsers;
using CXEX.Core.Constants;

namespace CXEX.Studio.Services;

/// <summary>
/// The Studio's build orchestrator. Packaging/layout run IN-PROCESS through the
/// CXEX.Build library (ElfParser -> CXEXLayoutEngine -> CXEXWriter, and the disk
/// writer) - no subprocess, no reimplementation. Toolchain + emulator steps
/// (gcc/nasm/cmake/qemu) run through RunAsync, which streams every output line to
/// a log callback so the GUI shows live output. This is the front-end over the
/// libraries that replaces the batch/ps1 scripts.
/// </summary>
public sealed class BuildService
{
    /// <summary>Map a friendly type name to the CXEX type code (matches the CLI).</summary>
    public static ushort TypeCodeFor(string type) => type.ToLowerInvariant() switch
    {
        "kernel" => CXFlags.TYPE_KERNEL,
        "boot" => CXFlags.TYPE_BOOT,
        "os" => CXFlags.TYPE_OS,
        "user" => CXFlags.TYPE_USER,
        _ => 0
    };

    /// <summary>ELF -> CXEX, entirely in-process via CXEX.Build. Throws on bad input.</summary>
    public void PackageCxex(string elfPath, string outPath, string type, Action<string>? log = null)
    {
        ushort code = TypeCodeFor(type);
        if (code == 0) throw new ArgumentException($"invalid CXEX type '{type}' (kernel|boot|os|user)");

        log?.Invoke($"package: {Path.GetFileName(elfPath)} -> {Path.GetFileName(outPath)} ({type})");
        byte[] elf = File.ReadAllBytes(elfPath);
        var (entry, segments) = ElfParser.Parse(elf);
        var layout = CXEXLayoutEngine.CreateLayout(entry, segments, code);
        CXEXWriter.WriteExecutable(outPath, layout);
        log?.Invoke($"package: wrote {outPath} (entry 0x{entry:X8})");
    }

    /// <summary>Build a unified XBPT disk from prebuilt artifacts, in-process via CXEX.Build.</summary>
    public void BuildDiskImage(string outPath, int diskMb, int bootMb,
                               byte[] stage1, byte[] stage2, byte[] kernel,
                               System.Collections.Generic.List<StagedFile> staged,
                               Action<string>? log = null)
    {
        log?.Invoke($"disk: laying out {diskMb} MB image");
        var map = DiskGeometryEngine.CalculateLayout(diskMb, bootMb, stage2.Length,
                                                     SumStaged(staged));
        XBPTImageWriter.WriteImage(outPath, map, stage1, stage2, kernel, staged);
        log?.Invoke($"disk: wrote {outPath}");
    }

    private static int SumStaged(System.Collections.Generic.List<StagedFile> staged)
    {
        int n = 0; foreach (var s in staged) n += s.Data?.Length ?? 0; return n;
    }

    /// <summary>
    /// Run an external tool (gcc/nasm/cmake/qemu), streaming stdout+stderr lines to
    /// `log` as they arrive. Returns the exit code; -1 if the process failed to start.
    /// Cancelable: cancellation kills the process.
    /// </summary>
    public async Task<int> RunAsync(string exe, string args, string workingDir,
                                    Action<string> log, CancellationToken ct = default)
    {
        log($"$ {exe} {args}");
        var psi = new ProcessStartInfo
        {
            FileName = exe,
            Arguments = args,
            WorkingDirectory = string.IsNullOrEmpty(workingDir) ? Environment.CurrentDirectory : workingDir,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };

        using var proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
        proc.OutputDataReceived += (_, e) => { if (e.Data != null) log(e.Data); };
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) log(e.Data); };

        try
        {
            if (!proc.Start()) { log($"error: failed to start '{exe}'"); return -1; }
        }
        catch (Exception ex) { log($"error: cannot launch '{exe}': {ex.Message}"); return -1; }

        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();

        using (ct.Register(() => { try { if (!proc.HasExited) proc.Kill(true); } catch { } }))
        {
            await proc.WaitForExitAsync(ct).ConfigureAwait(false);
        }
        log($"-> exit {proc.ExitCode}");
        return proc.ExitCode;
    }
}