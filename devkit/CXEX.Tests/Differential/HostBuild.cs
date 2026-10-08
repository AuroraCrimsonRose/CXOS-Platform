// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Diagnostics;
using System.Text;

namespace CXEX.Tests.Differential;

/// <summary>The result of running one external process.</summary>
public sealed record ProcResult(int ExitCode, string StdOut, string StdErr)
{
    public string All => StdOut + StdErr;
}

/// <summary>
/// Running X on the host, which is what every differential suite is built on.
///
/// <para>The X compiler emits 32-bit assembly whose entry point is
/// <c>_start</c>. To run it as an ordinary host process we rename that to
/// <c>main</c> and link it against a stub that hands Linux's argc/argv over
/// and exits through <c>int 0x80</c>. No VM and no CXK boot is involved — this
/// is why the suites need an ELF host rather than merely a compiler.</para>
///
/// <para>The link uses <b>clang</b>, not gcc (HARDENING_PLAN D5). With
/// <c>-nostdlib</c> no 32-bit libc is involved, so it needs no multilib.
/// <c>CXOS_HOSTCC</c> overrides it for a host that only has gcc.</para>
/// </summary>
public static class HostBuild
{
    /// <summary>Hands Linux's argc/argv to main, then exits with its return value.</summary>
    public const string ArgvStub =
        ".text\n.globl _start\n_start:\n    mov (%esp), %eax\n    lea 4(%esp), %ecx\n" +
        "    push %ecx\n    push %eax\n    call main\n    mov %eax, %ebx\n    mov $1, %eax\n    int $0x80\n";

    /// <summary>For programs that take no arguments.</summary>
    public const string PlainStub =
        ".text\n.globl _start\n_start:\n    call main\n    mov %eax, %ebx\n    mov $1, %eax\n    int $0x80\n";

    public static string HostCc =>
        Environment.GetEnvironmentVariable("CXOS_HOSTCC") is { Length: > 0 } cc ? cc : "clang";

    public static ProcResult Run(string exe, IEnumerable<string> args,
                                 string? workingDirectory = null, int timeoutMs = 1_800_000)
    {
        var psi = new ProcessStartInfo
        {
            FileName = exe,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = workingDirectory ?? Environment.CurrentDirectory,
        };
        foreach (string a in args) psi.ArgumentList.Add(a);

        using var p = Process.Start(psi) ?? throw new InvalidOperationException($"could not start {exe}");

        // Read both pipes concurrently. Draining one then the other deadlocks
        // as soon as the child fills the pipe it is not being read from, and
        // these tests move megabytes of dump output.
        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        p.OutputDataReceived += (_, e) => { if (e.Data is not null) stdout.AppendLine(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data is not null) stderr.AppendLine(e.Data); };
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();

        if (!p.WaitForExit(timeoutMs))
        {
            try { p.Kill(entireProcessTree: true); } catch { /* already gone */ }
            throw new TimeoutException($"{exe} did not finish within {timeoutMs} ms");
        }
        p.WaitForExit();   // flushes the async readers

        return new ProcResult(p.ExitCode, stdout.ToString(), stderr.ToString());
    }

    /// <summary>
    /// Links X assembly into a native 32-bit binary, renaming the entry point
    /// from <c>_start</c> to <c>main</c> so the stub can call it.
    ///
    /// <para>The rename goes to a <c>.host.s</c> sidecar and <b>never touches
    /// <paramref name="asmPath"/></b>. That matters beyond tidiness: the
    /// self-hosting test compares the assembly of three compiler stages, and
    /// rewriting the file it is about to compare silently changes the thing
    /// under test.</para>
    /// </summary>
    public static void Link(string asmPath, string exePath, string stub, string workDir)
    {
        string text = File.ReadAllText(asmPath);
        int at = text.IndexOf(".globl _start", StringComparison.Ordinal);
        if (at >= 0)
            text = string.Concat(text.AsSpan(0, at), ".globl main", text.AsSpan(at + ".globl _start".Length));

        string hostAsm = exePath + ".host.s";
        File.WriteAllText(hostAsm, text);

        string stubPath = Path.Combine(workDir, ReferenceEquals(stub, ArgvStub) || stub == ArgvStub
            ? "hstub.s" : "stub.s");
        if (!File.Exists(stubPath)) File.WriteAllText(stubPath, stub);

        var r = Run(HostCc,
                    new[] { "-m32", "-nostdlib", "-static", "-o", exePath, hostAsm, stubPath },
                    workDir);
        if (r.ExitCode != 0)
            throw new InvalidOperationException($"{HostCc} failed to link {Path.GetFileName(asmPath)}:\n{r.All}");
    }

    /// <summary>
    /// Compiles one of <c>os/xc</c>'s dump programs for the host and links it.
    /// Returns the path to the runnable binary.
    /// </summary>
    public static string BuildDumpProgram(string cxk, string repo, string osDir, string prog, string workDir)
    {
        string baseName = Path.Combine(workDir, prog);
        var r = Run(cxk, new[]
        {
            "compile", Path.Combine(osDir, "xc", prog + ".xfxn"), baseName + ".elf",
            "--no-prelude", "--emit-asm",
            "-I", Path.Combine(osDir, "xc", "host"),
            "-I", Path.Combine(osDir, "xc"),
            "-I", Path.Combine(osDir, "std"),
        }, repo);

        if (!File.Exists(baseName + ".s"))
            throw new InvalidOperationException($"{prog} did not compile:\n{r.All}");

        Link(baseName + ".s", baseName, ArgvStub, workDir);
        return baseName;
    }

    /// <summary>
    /// The <c>os/</c> directory holding <c>xc/&lt;prog&gt;.xfxn</c>. In this
    /// repository that is simply <c>os/</c>; the second candidate is kept for a
    /// CXK_ROOT pointing at a tree that still nests it.
    /// </summary>
    public static string? FindOsDir(string repoRoot, string prog)
    {
        foreach (string d in new[] { Path.Combine(repoRoot, "os"), Path.Combine(repoRoot, "CXK", "os") })
            if (File.Exists(Path.Combine(d, "xc", prog + ".xfxn")))
                return d;
        return null;
    }

    /// <summary>A scratch directory that deletes itself, unless KEEP is set.</summary>
    public sealed class Workspace : IDisposable
    {
        public string Path { get; }

        public Workspace(string prefix)
        {
            Path = System.IO.Path.Combine(System.IO.Path.GetTempPath(),
                                          prefix + "-" + Guid.NewGuid().ToString("N")[..8]);
            Directory.CreateDirectory(Path);
        }

        public void Dispose()
        {
            if (Environment.GetEnvironmentVariable("KEEP") is { Length: > 0 })
            {
                Console.WriteLine($"kept {Path}");
                return;
            }
            try { Directory.Delete(Path, recursive: true); } catch (IOException) { /* best effort */ }
        }
    }
}
