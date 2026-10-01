using System.Collections.Generic;
using System.Text;
using CXEX.CLI.Infrastructure;
using Spectre.Console;

namespace CXEX.CLI.Wrappers;

/// <summary>
/// CMake, driven with the Ninja generator on every host (HARDENING_PLAN D5).
/// Ninja replaces NMake Makefiles, so a build no longer needs the MSVC
/// Developer Command Prompt - or any MSVC at all.
/// </summary>
public static class CMakeTool
{
    public const string Generator = "Ninja";

    /// <summary>Configure <paramref name="buildDir"/> from <paramref name="sourceDir"/>, passing each -D flag through verbatim.</summary>
    public static bool Configure(string sourceDir, string buildDir, IEnumerable<string> defines)
    {
        var args = new StringBuilder();
        args.Append($"-S \"{sourceDir}\" -B \"{buildDir}\" -G \"{Generator}\" ");
        foreach (var d in defines) args.Append($"{d} ");

        return ProcessRunner.Run("cmake", args.ToString().Trim()) == 0;
    }

    /// <summary>Build every target in <paramref name="buildDir"/>. <paramref name="jobs"/> of 0 lets Ninja pick.</summary>
    public static bool Build(string buildDir, int jobs = 0)
    {
        string args = $"--build \"{buildDir}\"";
        if (jobs > 0) args += $" -j {jobs}";

        return ProcessRunner.Run("cmake", args) == 0;
    }
}
