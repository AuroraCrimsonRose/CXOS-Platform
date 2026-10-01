using System;
using System.IO;

namespace CXEX.CLI.Infrastructure;

/// <summary>
/// Turns a tool's plain name into something the OS will actually launch.
///
/// <para>This exists for one name: <c>ld.lld</c>. Windows resolves a bare name
/// against PATH and appends the PATHEXT extensions - but only when the name has
/// no extension of its own. <c>ld.lld</c> appears to have one already (".lld"),
/// so CreateProcess looks for a file called exactly "ld.lld", never finds
/// <c>ld.lld.exe</c>, and reports the linker as missing on a machine where it is
/// installed and on PATH.</para>
///
/// <para>cmd.exe gets this right, which is why the same linker resolves fine
/// from a shell and from Ninja's command lines, and only fails when the DevKit
/// starts it directly.</para>
/// </summary>
public static class ExecutableResolver
{
    /// <summary>
    /// The full path to <paramref name="name"/> if it can be found on PATH, or
    /// <paramref name="name"/> unchanged - leaving the OS to resolve it, and to
    /// report the failure if it cannot.
    /// </summary>
    public static string Resolve(string name)
    {
        if (string.IsNullOrWhiteSpace(name)) return name;
        if (!OperatingSystem.IsWindows()) return name;
        if (Path.IsPathRooted(name)) return name;

        string[] extensions = Path.GetExtension(name).Equals(".exe", StringComparison.OrdinalIgnoreCase)
            ? new[] { string.Empty }
            : new[] { ".exe", ".cmd", ".bat", string.Empty };

        string pathVariable = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;

        foreach (string dir in pathVariable.Split(Path.PathSeparator))
        {
            if (string.IsNullOrWhiteSpace(dir)) continue;

            foreach (string ext in extensions)
            {
                string candidate;
                try { candidate = Path.Combine(dir.Trim(), name + ext); }
                catch (ArgumentException) { break; }   // a malformed PATH entry: skip the whole directory

                if (File.Exists(candidate)) return candidate;
            }
        }

        return name;
    }
}
