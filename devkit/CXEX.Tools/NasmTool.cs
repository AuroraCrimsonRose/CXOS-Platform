// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
namespace CXEX.Tools;

/// <summary>
/// NASM, for the boot sector and stage 2. They are NASM syntax and build the
/// same on every host, which is why NASM stays while the C toolchain moved to
/// LLVM (HARDENING_PLAN D5).
/// </summary>
public static class NasmTool
{
    public static ToolResult Assemble(string sourceFile, string outputFile,
                                      string format = "elf32",
                                      IEnumerable<string>? includeDirs = null,
                                      ToolRunOptions? options = null)
    {
        var args = new List<string> { "-f", format };

        if (includeDirs is not null)
            foreach (string inc in includeDirs)
            {
                // NASM wants a trailing separator on an include directory: with
                // -Ifoo it concatenates, so "foo" + "bar.inc" becomes
                // "foobar.inc". Added here rather than asked of every caller.
                string dir = inc.EndsWith(Path.DirectorySeparatorChar) || inc.EndsWith('/')
                    ? inc
                    : inc + Path.DirectorySeparatorChar;
                args.Add($"-I{dir}");
            }

        args.Add("-o"); args.Add(outputFile);
        args.Add(sourceFile);

        return ToolProcess.Run("nasm", args, options);
    }
}
