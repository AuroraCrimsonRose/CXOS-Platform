using System.Collections.Generic;
using System.Text;
using CXEX.CLI.Infrastructure;
using Spectre.Console;

namespace CXEX.CLI.Wrappers;

public static class NasmTool
{
    public static bool Assemble(string sourceFile, string outputFile, string format = "elf32", IEnumerable<string>? includeDirs = null)
    {
        AnsiConsole.MarkupLine($"[cyan]NASM ({format}):[/] {System.IO.Path.GetFileName(sourceFile)}");

        var args = new StringBuilder();
        args.Append($"-f {format} ");

        if (includeDirs != null)
        {
            foreach (var inc in includeDirs)
            {
                // NASM requires a trailing slash for include directories sometimes, but -I works
                args.Append($"-I\"{inc}\" ");
            }
        }

        args.Append($"-o \"{outputFile}\" \"{sourceFile}\"");

        return ProcessRunner.Run("nasm", args.ToString().Trim()) == 0;
    }
}