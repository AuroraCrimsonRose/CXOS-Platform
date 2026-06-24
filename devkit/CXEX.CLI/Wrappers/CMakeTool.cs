using CXEX.CLI.Infrastructure;
using Spectre.Console;

namespace CXEX.CLI.Wrappers;

public static class CMakeTool
{
    public static bool Configure(string sourceDir, string buildDir, bool signArtifacts = false)
    {
        AnsiConsole.MarkupLine("[cyan]Configuring CMake...[/]");
        string signFlag = signArtifacts ? "-DSIGN=ON" : "-DSIGN=OFF";
        string args = $"\"{sourceDir}\" -B \"{buildDir}\" -G \"NMake Makefiles\" {signFlag}";

        return ProcessRunner.Run("cmake", args) == 0;
    }

    public static bool Build(string buildDir)
    {
        AnsiConsole.MarkupLine("[cyan]Building CMake targets...[/]");
        return ProcessRunner.Run("cmake", $"--build \"{buildDir}\"") == 0;
    }
}