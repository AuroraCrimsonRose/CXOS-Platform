// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
namespace CXEX.Tools;

/// <summary>
/// Runs a tool purely to find out whether it is there and what version it is.
/// Unlike a build step it streams nothing: a pre-flight probe that echoed
/// every tool's banner would bury the build log before the build started.
/// </summary>
public static class ToolProbe
{
    /// <summary>
    /// Runs <paramref name="executable"/> and returns its first line of output,
    /// or null if it could not be started at all. A non-zero exit still returns
    /// the output: some tools report their version and exit non-zero.
    /// </summary>
    /// <remarks>
    /// The timeout is short on purpose. A version probe that has not answered
    /// in ten seconds is not going to, and a pre-flight that hangs is worse
    /// than one that reports a tool as missing - previously there was no
    /// timeout here at all, so a wedged tool hung the build before it started.
    /// </remarks>
    public static string? FirstLine(string executable, params string[] arguments)
    {
        var result = ToolProcess.Run(executable, arguments,
            new ToolRunOptions { Timeout = TimeSpan.FromSeconds(10) });

        if (result.Outcome == ToolOutcome.NotFound) return null;
        if (result.Outcome == ToolOutcome.TimedOut) return null;

        string text = string.IsNullOrWhiteSpace(result.StandardOutput)
            ? result.StandardError
            : result.StandardOutput;

        foreach (string line in text.Split('\n'))
            if (!string.IsNullOrWhiteSpace(line)) return line.Trim();

        return string.Empty;
    }
}
