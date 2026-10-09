// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Diagnostics;
using System.Text;

namespace CXEX.Tools;

/// <summary>What became of an attempt to run an external tool.</summary>
public enum ToolOutcome
{
    /// <summary>The tool ran to completion and exited zero.</summary>
    Succeeded,
    /// <summary>The tool ran to completion and exited non-zero. <see cref="ToolResult.ExitCode"/> is its code.</summary>
    Failed,
    /// <summary>The executable could not be started - not on PATH, not executable, or no such file.</summary>
    NotFound,
    /// <summary>The tool was still running when its budget ran out, and was killed.</summary>
    TimedOut,
}

/// <summary>
/// The result of running an external tool. A record rather than an
/// <c>int</c> because the outcomes are genuinely different kinds of thing
/// and a caller needs to tell them apart (engineering §10).
///
/// <para>The previous runner returned an <c>int</c> and used <c>-1</c> for
/// "could not launch" - which is also a perfectly ordinary exit code for a
/// tool that ran and failed, so "clang is not installed" and "clang rejected
/// your source" were indistinguishable to every caller.</para>
/// </summary>
public sealed record ToolResult(
    ToolOutcome Outcome,
    int ExitCode,
    string Executable,
    string ResolvedExecutable,
    IReadOnlyList<string> Arguments,
    string StandardOutput,
    string StandardError,
    TimeSpan Duration,
    string? Diagnostic = null)
{
    public bool Ok => Outcome == ToolOutcome.Succeeded;

    /// <summary>One line naming what went wrong, suitable for showing a user.</summary>
    public string Describe() => Outcome switch
    {
        ToolOutcome.Succeeded => $"{Executable} succeeded",
        ToolOutcome.Failed => $"{Executable} exited {ExitCode}",
        ToolOutcome.NotFound => $"{Executable} could not be started: {Diagnostic ?? "not found on PATH"}",
        ToolOutcome.TimedOut => $"{Executable} did not finish within {Duration.TotalSeconds:0.#}s and was killed",
        _ => $"{Executable}: unknown outcome",
    };

    /// <summary>The command line as it would be typed, for logs and --dry-run.</summary>
    public string CommandLine() => ToolProcess.Quote(Executable, Arguments);
}

/// <summary>How to run a tool. Every field has a stated default, because
/// §9's complaint was that these were all implicit.</summary>
public sealed class ToolRunOptions
{
    /// <summary>
    /// The directory the tool runs in. Null means the current directory -
    /// stated rather than assumed, since a build tool's relative paths all
    /// resolve against it.
    /// </summary>
    public string? WorkingDirectory { get; set; }

    /// <summary>
    /// How long the tool gets. The default is 30 minutes: long enough for a
    /// full kernel build, short enough that a wedged tool eventually reports
    /// instead of hanging a session. There was previously no timeout at all -
    /// a bare WaitForExit() - so a tool that blocked on input or on a lock
    /// hung the build with no indication of which tool or why.
    /// <para>TimeSpan.Zero or a negative span means wait indefinitely, which
    /// a caller must ask for explicitly.</para>
    /// </summary>
    public TimeSpan Timeout { get; set; } = TimeSpan.FromMinutes(30);

    /// <summary>
    /// Environment variables to set for the child, on top of this process's.
    /// The child otherwise inherits the parent's environment: that is what
    /// makes PATH, and therefore tool discovery, work at all.
    /// </summary>
    public IReadOnlyDictionary<string, string>? Environment { get; set; }

    /// <summary>
    /// Environment variables to REMOVE from the child's inherited set. For a
    /// build that must not be steered by ambient state - CC, LD, CFLAGS and
    /// friends - rather than trusting the tool to ignore them.
    /// </summary>
    public IReadOnlyList<string>? ClearEnvironment { get; set; }

    /// <summary>Called for each line of stdout, as it arrives. Null discards.</summary>
    public Action<string>? OnOutput { get; set; }

    /// <summary>Called for each line of stderr, as it arrives. Null discards.</summary>
    public Action<string>? OnError { get; set; }
}

/// <summary>
/// Runs external tools: clang, ld.lld, nasm, cmake, qemu, bochs.
///
/// <para>This lives in CXEX.Tools rather than CXEX.CLI for two reasons that
/// turn out to be one. DevKit engineering §6 asks for shared tool execution
/// to be a library; §8 asks that Studio not reference CXEX.CLI. Studio was
/// calling <c>QemuTool</c> and <c>BochsTool</c>, which lived in the CLI, so
/// it referenced the whole command-line front end - Spectre.Console, the
/// command registrar and all - to launch an emulator. Moving the runner is
/// what let that reference go.</para>
///
/// <para>It therefore depends on nothing but the BCL. Output is delivered
/// through callbacks instead of being written to a console, because a library
/// that writes to <c>AnsiConsole</c> is a front end wearing a library's
/// name.</para>
///
/// <para>EXECUTION RULES (DevKit security review §9)</para>
/// <list type="bullet">
/// <item><b>Arguments are a list, never a string.</b> Each argument is passed
/// through <c>ProcessStartInfo.ArgumentList</c>, so the runtime does the
/// quoting and no caller hand-rolls it. Every wrapper used to build one
/// command string with its own <c>\"</c> escaping - which breaks on a path
/// containing a quote, and silently splits one containing a space if an
/// escape is forgotten.</item>
/// <item><b>No shell.</b> <c>UseShellExecute</c> is false, always, so there
/// is no shell to parse metacharacters in a path.</item>
/// <item><b>Discovery is explicit</b> - see <see cref="ExecutableResolver"/>.</item>
/// <item><b>The working directory is explicit</b>, defaulting to the current
/// directory.</item>
/// <item><b>The environment is inherited</b>, with additions and removals
/// stated per call.</item>
/// <item><b>Every run has a timeout</b> and a killed tool is reported as
/// TimedOut, not as a failure of the tool.</item>
/// <item><b>stdout and stderr are captured and streamed</b>, both, and kept
/// on the result so a caller can report rather than re-run.</item>
/// <item><b>Exit codes are interpreted, not returned raw</b>: see
/// <see cref="ToolOutcome"/>.</item>
/// </list>
/// </summary>
public static class ToolProcess
{
    private static readonly ToolRunOptions Defaults = new();

    public static ToolResult Run(string executable, IEnumerable<string> arguments,
                                 ToolRunOptions? options = null)
    {
        options ??= Defaults;
        var args = arguments as IReadOnlyList<string> ?? arguments.ToList();
        string resolved = ExecutableResolver.Resolve(executable);

        var startInfo = new ProcessStartInfo
        {
            FileName = resolved,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            // stdin is redirected and closed immediately: a tool that decides
            // to prompt gets EOF and exits, instead of waiting forever on a
            // console nobody is watching.
            RedirectStandardInput = true,
            CreateNoWindow = true,
            WorkingDirectory = options.WorkingDirectory is { Length: > 0 } wd
                ? wd
                : System.Environment.CurrentDirectory,
        };

        foreach (string a in args) startInfo.ArgumentList.Add(a);

        if (options.ClearEnvironment is not null)
            foreach (string key in options.ClearEnvironment)
                startInfo.Environment.Remove(key);

        if (options.Environment is not null)
            foreach (var (key, value) in options.Environment)
                startInfo.Environment[key] = value;

        var stdout = new StringBuilder();
        var stderr = new StringBuilder();
        var clock = Stopwatch.StartNew();

        try
        {
            using var process = new Process { StartInfo = startInfo };

            process.OutputDataReceived += (_, e) =>
            {
                if (e.Data is null) return;
                stdout.AppendLine(e.Data);
                options.OnOutput?.Invoke(e.Data);
            };
            process.ErrorDataReceived += (_, e) =>
            {
                if (e.Data is null) return;
                stderr.AppendLine(e.Data);
                options.OnError?.Invoke(e.Data);
            };

            process.Start();
            process.StandardInput.Close();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();

            bool finished = options.Timeout > TimeSpan.Zero
                ? process.WaitForExit((int)options.Timeout.TotalMilliseconds)
                : WaitForever(process);

            if (!finished)
            {
                // Kill the whole tree: cmake and qemu both spawn children, and
                // killing only the parent leaves them holding the build
                // directory or the disk image.
                try { process.Kill(entireProcessTree: true); } catch { /* already gone */ }
                clock.Stop();
                return new ToolResult(ToolOutcome.TimedOut, -1, executable, resolved, args,
                                      stdout.ToString(), stderr.ToString(), clock.Elapsed,
                                      $"exceeded {options.Timeout.TotalSeconds:0.#}s");
            }

            // Lets the async readers drain: WaitForExit(int) returning true
            // does not guarantee the output callbacks have all fired, and
            // dropping the last lines of a compiler error is the worst
            // possible thing to drop.
            process.WaitForExit();
            clock.Stop();

            return new ToolResult(
                process.ExitCode == 0 ? ToolOutcome.Succeeded : ToolOutcome.Failed,
                process.ExitCode, executable, resolved, args,
                stdout.ToString(), stderr.ToString(), clock.Elapsed);
        }
        catch (Exception ex)
        {
            // A Win32Exception here means the executable could not be started
            // at all, which is a different answer from any exit code and is
            // reported as such rather than as -1.
            clock.Stop();
            return new ToolResult(ToolOutcome.NotFound, -1, executable, resolved, args,
                                  stdout.ToString(), stderr.ToString(), clock.Elapsed, ex.Message);
        }
    }

    private static bool WaitForever(Process p) { p.WaitForExit(); return true; }

    /// <summary>
    /// Renders a command line the way a shell would need it written. For
    /// logging and <c>--dry-run</c> only - never for building the arguments
    /// actually passed, which go through ArgumentList unquoted.
    /// </summary>
    public static string Quote(string executable, IEnumerable<string> arguments)
    {
        var sb = new StringBuilder(QuoteOne(executable));
        foreach (string a in arguments) sb.Append(' ').Append(QuoteOne(a));
        return sb.ToString();
    }

    private static string QuoteOne(string s)
    {
        if (s.Length == 0) return "\"\"";
        if (!s.Any(c => c is ' ' or '\t' or '"')) return s;
        return '"' + s.Replace("\"", "\\\"") + '"';
    }
}
