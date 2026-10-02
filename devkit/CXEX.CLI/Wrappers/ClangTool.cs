// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Text;
using CXEX.CLI.Infrastructure;
using Spectre.Console;

namespace CXEX.CLI.Wrappers;

/// <summary>
/// The LLVM cross toolchain: clang's integrated assembler and ld.lld, targeting
/// i686-elf. Replaces the i686-elf GCC wrapper this used to be (HARDENING_PLAN
/// D5) - one toolchain on every host, with no cross-GCC to install.
///
/// Both tools are resolved by name off PATH. CXK_CLANG and CXK_LLD override
/// that, for a host carrying several LLVM versions or one where they are not on
/// PATH at all.
/// </summary>
public static class ClangTool
{
    /// <summary>The target every CXK artifact is built for: 32-bit x86, bare metal, ELF.</summary>
    public const string Target = "i686-elf";

    private static string Clang => Environment.GetEnvironmentVariable("CXK_CLANG") is { Length: > 0 } c ? c : "clang";
    private static string Lld   => Environment.GetEnvironmentVariable("CXK_LLD")   is { Length: > 0 } l ? l : "ld.lld";

    /// <summary>
    /// Assemble or compile one source to a relocatable object with clang's
    /// integrated assembler. The emitter's .s is GNU-syntax assembly; checked
    /// against GNU as on xc's own 70,000 lines, the resulting .text is
    /// byte-identical (D5).
    /// </summary>
    public static bool Compile(string sourceFile, string outputFile, IEnumerable<string>? includeDirs = null, string extraFlags = "")
    {
        AnsiConsole.MarkupLine($"[cyan]clang (compile):[/] {System.IO.Path.GetFileName(sourceFile)}");

        var args = new StringBuilder();
        // Mandatory flags for freestanding CXK code. --target replaces GCC's -m32:
        // the triple already fixes the architecture, so passing both is redundant.
        args.Append($"--target={Target} -ffreestanding -fno-pic -fno-stack-protector -Wall -Wextra -c ");

        if (includeDirs != null)
        {
            foreach (var inc in includeDirs) args.Append($"-I\"{inc}\" ");
        }

        if (!string.IsNullOrEmpty(extraFlags)) args.Append($"{extraFlags} ");

        args.Append($"-o \"{outputFile}\" \"{sourceFile}\"");

        return ProcessRunner.Run(Clang, args.ToString().Trim()) == 0;
    }

    /// <summary>
    /// Link objects against a linker script with ld.lld.
    ///
    /// ld.lld is called directly rather than through the clang driver: there is
    /// no C runtime to find and nothing to pull in from a sysroot, so the driver
    /// would only add flags this has to cancel. -m elf_i386 is explicit because
    /// the emitted layout must not depend on lld inferring it from the first
    /// object it happens to read.
    /// </summary>
    public static bool Link(IEnumerable<string> objectFiles, string outputFile, string linkerScript, string extraFlags = "")
    {
        AnsiConsole.MarkupLine($"[cyan]ld.lld (link):[/] {System.IO.Path.GetFileName(outputFile)}");

        var args = new StringBuilder();
        args.Append($"-m elf_i386 -nostdlib -T \"{linkerScript}\" ");

        if (!string.IsNullOrEmpty(extraFlags)) args.Append($"{extraFlags} ");

        args.Append($"-o \"{outputFile}\" ");

        foreach (var obj in objectFiles) args.Append($"\"{obj}\" ");

        return ProcessRunner.Run(Lld, args.ToString().Trim()) == 0;
    }
}
