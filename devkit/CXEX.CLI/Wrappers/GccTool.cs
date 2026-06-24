using System.Collections.Generic;
using System.Text;
using CXEX.CLI.Infrastructure;
using Spectre.Console;

namespace CXEX.CLI.Wrappers;

public static class GccTool
{
    // If i686-elf-gcc is not in your global PATH, you can hardcode the absolute path here:
    // private const string Executable = @"C:\cross\i686-elf\bin\i686-elf-gcc.exe";
    private const string Executable = "i686-elf-gcc";

    public static bool Compile(string sourceFile, string outputFile, IEnumerable<string>? includeDirs = null, string extraFlags = "")
    {
        AnsiConsole.MarkupLine($"[cyan]GCC (Compile):[/] {System.IO.Path.GetFileName(sourceFile)}");

        var args = new StringBuilder();
        // Mandatory OS flags for freestanding C
        args.Append("-ffreestanding -fno-pic -fno-stack-protector -m32 -Wall -Wextra -c ");

        if (includeDirs != null)
        {
            foreach (var inc in includeDirs) args.Append($"-I\"{inc}\" ");
        }

        if (!string.IsNullOrEmpty(extraFlags)) args.Append($"{extraFlags} ");

        args.Append($"-o \"{outputFile}\" \"{sourceFile}\"");

        return ProcessRunner.Run(Executable, args.ToString().Trim()) == 0;
    }

    public static bool Link(IEnumerable<string> objectFiles, string outputFile, string linkerScript, string extraFlags = "")
    {
        AnsiConsole.MarkupLine($"[cyan]GCC (Link):[/] {System.IO.Path.GetFileName(outputFile)}");

        var args = new StringBuilder();
        // Mandatory OS flags for linking
        args.Append("-ffreestanding -fno-pic -no-pie -fno-stack-protector -nostdlib ");
        args.Append($"-Wl,-T,\"{linkerScript}\" ");

        if (!string.IsNullOrEmpty(extraFlags)) args.Append($"{extraFlags} ");

        args.Append($"-o \"{outputFile}\" ");

        foreach (var obj in objectFiles) args.Append($"\"{obj}\" ");

        return ProcessRunner.Run(Executable, args.ToString().Trim()) == 0;
    }
}