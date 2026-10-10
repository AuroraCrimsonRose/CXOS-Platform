// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
namespace CXEX.Tools;

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
    public static ToolResult Compile(string sourceFile, string outputFile,
                                     IEnumerable<string>? includeDirs = null,
                                     IEnumerable<string>? extraFlags = null,
                                     ToolRunOptions? options = null)
    {
        var args = new List<string>
        {
            // Mandatory flags for freestanding CXK code. --target replaces GCC's
            // -m32: the triple already fixes the architecture, so passing both
            // is redundant.
            $"--target={Target}", "-ffreestanding", "-fno-pic",
            "-fno-stack-protector", "-Wall", "-Wextra", "-c",
        };

        if (includeDirs is not null)
            foreach (string inc in includeDirs) args.Add($"-I{inc}");

        if (extraFlags is not null) args.AddRange(extraFlags);

        args.Add("-o"); args.Add(outputFile);
        args.Add(sourceFile);

        return ToolProcess.Run(Clang, args, options);
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
    public static ToolResult Link(IEnumerable<string> objectFiles, string outputFile,
                                  string linkerScript,
                                  IEnumerable<string>? extraFlags = null,
                                  ToolRunOptions? options = null)
    {
        var args = new List<string> { "-m", "elf_i386", "-nostdlib", "-T", linkerScript };

        if (extraFlags is not null) args.AddRange(extraFlags);

        args.Add("-o"); args.Add(outputFile);
        args.AddRange(objectFiles);

        return ToolProcess.Run(Lld, args, options);
    }
}
