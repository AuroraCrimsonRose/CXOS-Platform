// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
namespace CXEX.Tools;

/// <summary>
/// CMake, driven with the Ninja generator on every host (HARDENING_PLAN D5).
/// Ninja replaces NMake Makefiles, so a build no longer needs the MSVC
/// Developer Command Prompt - or any MSVC at all.
/// </summary>
public static class CMakeTool
{
    public const string Generator = "Ninja";

    /// <summary>Configure <paramref name="buildDir"/> from <paramref name="sourceDir"/>, passing each -D flag through verbatim.</summary>
    public static ToolResult Configure(string sourceDir, string buildDir,
                                       IEnumerable<string> defines,
                                       ToolRunOptions? options = null)
    {
        var args = new List<string> { "-S", sourceDir, "-B", buildDir, "-G", Generator };
        args.AddRange(defines);
        return ToolProcess.Run("cmake", args, options);
    }

    /// <summary>Build every target in <paramref name="buildDir"/>. <paramref name="jobs"/> of 0 lets Ninja pick.</summary>
    public static ToolResult Build(string buildDir, int jobs = 0, ToolRunOptions? options = null)
    {
        var args = new List<string> { "--build", buildDir };
        if (jobs > 0) { args.Add("-j"); args.Add(jobs.ToString()); }
        return ToolProcess.Run("cmake", args, options);
    }
}
