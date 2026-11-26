// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Reflection;
using System;
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.CLI.Commands;

// Initialize the Spectre Command App
var app = new CommandApp();

app.Configure(config =>
{
    config.SetApplicationName("cxk");
    // From the assembly, which devkit/Directory.Build.props sets from
    // versions.json. It was the hardcoded string "5.0.0" - a number nothing
    // checked, which is why it disagreed with the VSIX's 0.5.0 and with the
    // 1.0.0 every other assembly reported.
    config.SetApplicationVersion(
        typeof(Program).Assembly
            .GetCustomAttribute<System.Reflection.AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion.Split('+')[0]
        ?? typeof(Program).Assembly.GetName().Version?.ToString(3)
        ?? "unknown");

    // ---- key management (replaces makekeys.py / embedkey.py) ----
    config.AddCommand<KeygenCommand>("keygen")
        .WithDescription("Generates an RSA signing keypair (.xksk private + .xkpk public).");

    config.AddCommand<EmbedCommand>("embed")
        .WithDescription("Converts a binary file into a C header byte array (e.g. trusted_key.c, app_image.h).");

    // ---- X toolchain (CXEX.Lang) ----
    config.AddCommand<CompileCommand>("compile")
        .WithDescription("Compiles an X source file (.xfxn) into an ELF, ready for `build` to package as CXEX.");

    // ---- packaging (replaces mkcxes.py) ----
    config.AddCommand<BuildCommand>("build")
        .WithDescription("Compiles an ELF binary into a CXEX executable (.xkex, .xbex, .xoex, .xsex, .xuex).");

    config.AddCommand<SignCommand>("sign")
        .WithDescription("Appends a CXSG cryptographic signature block to a CXEX image.");

    // ---- imaging (replaces mkdisk.py / pad_*.ps1) ----
    config.AddCommand<ImageCommand>("image")
        .WithDescription("Compiles stage1, stage2, the kernel, and the CXFS payload into a bootable XBPT disk image.");

    // ---- run + inspect ----
    config.AddCommand<RunCommand>("run")
        .WithDescription("Boots a CXK disk image in QEMU (replaces run_qemu.bat).");

    config.AddCommand<VerifyBootCommand>("verify-boot")
        .WithDescription("Boots an image headless and reports PASS/FAIL from the serial console (replaces serialboot.ps1, bootverify.py).");

    config.AddCommand<VerifyHyperVCommand>("verify-hyperv")
        .WithDescription("Boots a VHDX copy of an image in a Hyper-V Gen 1 VM (AMI BIOS, IDE) and reports PASS/FAIL from COM1, as verify-boot does.");

    config.AddCommand<KsymsCommand>("ksyms")
        .WithDescription("Writes an ELF64 kernel's function symbols as C, for the table a panic names its callers with.");

    config.AddCommand<VerifyCrashlogCommand>("verify-crashlog")
        .WithDescription("Boots a copy of an image twice: the first panics on request and must save its crash log, the second must recover it to /System/crash.log and clear the area.");

    config.AddCommand<VerifySeedCommand>("verify-seed")
        .WithDescription("Boots a copy of an image twice: the first must leave a random seed in the crash area, the second must credit it and leave another.");

    config.AddCommand<InspectCommand>("inspect")
        .WithDescription("Dumps a CXEX image's header, sections, and signature (replaces cxkdump.py).");

    // ---- validation ----
    config.AddCommand<CheckCommand>("check")
        .WithDescription("Validates that all source files listed in CMakeLists.txt exist.");

    config.AddCommand<CheckAbiCommand>("check-abi")
        .WithDescription("Validates that cxk_abi.h and the X ABI prelude are what abi/kernel.xfil generates.");

    config.AddBranch("abi", abi =>
    {
        abi.SetDescription("The kernel interface, abi/kernel.xfil.");
        abi.AddCommand<AbiGenerateCommand>("generate")
            .WithDescription("Writes abi/cxk_abi.h from abi/kernel.xfil.");
    });

    config.AddCommand<CheckVersionsCommand>("check-versions")
        .WithDescription("Validates that every version in versions.json matches the tree that declares it.");

    config.AddCommand<TokensCommand>("tokens")
        .WithDescription("Prints the tokens of X source files (compared against the lexer written in X).");

    config.AddCommand<AstCommand>("ast")
        .WithDescription("Prints the syntax trees of X source files (compared against the parser written in X).");

    config.AddCommand<SemaCommand>("sema")
        .WithDescription("Prints each expression's type and every diagnostic for X programs (compared against the type checker written in X).");

    config.AddCommand<AsmCommand>("asm")
        .WithDescription("Prints the assembly `compile` generates for X programs (compared against the code generator written in X).");

    config.AddCommand<IrCommand>("ir")
        .WithDescription("Prints the typed IR X programs lower to, checked by the IR verifier (docs: devkit/CXEX.Lang/IR/IR.md).");

    config.AddCommand<PreludeCommand>("prelude")
        .WithDescription("Prints the ABI prelude that `compile` puts in front of every program.");

    config.AddCommand<CheckXDataCommand>("check-xdata")
        .WithDescription("Validates X Data documents (service descriptors), with the same rules as the X reader.");

    // ---- every test, and every sabotage (roadmap 1.5d3) ----
    config.AddBranch("test", test =>
    {
        test.SetDescription("Runs the tests and the sabotages that show they can fail.");

        test.AddCommand<TestAllCommand>("all")
            .WithDescription("Builds and boots the OS in each variant, runs dotnet test (DevKit and kernel host tests), then the container's Differential run, into one summary.");
        test.AddCommand<TestSabotageCommand>("sabotage")
            .WithDescription("For each sabotage in a file: apply, build, boot, restore, and judge by which self-tests went red.");
    });

    // ---- the real machine ----
    config.AddBranch("rig", rig =>
    {
        rig.SetDescription("Tools for testing on real hardware.");

        rig.AddCommand<RigCrashlogCommand>("crashlog")
            .WithDescription("Prints the crash log a panic left in a disk's CRASH partition, without booting it.");
        rig.AddCommand<RigBootlogCommand>("bootlog")
            .WithDescription("Prints (or -o saves) the system log the last boot left in a disk's CRASH partition. Needs an elevated prompt for a disk.");
        rig.AddCommand<RigDisksCommand>("disks")
            .WithDescription("Lists this machine's disks, to find the stick's number (disk5). Needs no elevation.");
        rig.AddCommand<RigDumpCommand>("dump")
            .WithDescription("Copies a disk's CXOS part into an image file, read-only, for the image tools.");
        rig.AddCommand<RigHwreportCommand>("hwreport")
            .WithDescription("Prints (or -o saves) the hardware report the last boot left in a disk's CRASH partition. Needs an elevated prompt for a disk.");
    });

    // ---- the hardware report (roadmap 1.8b) ----
    config.AddBranch("hw", hw =>
    {
        hw.SetDescription("Reads and compares the kernel's hardware reports (docs/hardware/CX_HW_REPORT.md).");

        hw.AddCommand<HwShowCommand>("show")
            .WithDescription("Prints a report from a serial log, a saved report or a disk, each field by name, and what it says is wrong.");
        hw.AddCommand<HwDiffCommand>("diff")
            .WithDescription("Compares two reports fact by fact: expected differences (firmware-chosen addresses, hypervisor leaves, rates within 1%) apart from findings. Exit 1 on a finding.");
    });

    // ---- fonts: bitmap (v1) and outline (v2) ----
    config.AddBranch("font", font =>
    {
        font.SetDescription("Builds and inspects XFNT fonts: v1 bitmaps for the console, v2 outlines for the GUI.");

        font.AddCommand<FontBuildCommand>("build")
            .WithDescription("A BDF becomes a console v1 .xfnt for one character set (CP437), refusing a font missing any of it. A TrueType .ttf becomes a v2 outline .xfnt under a name of its own (--face), cut to the characters it maps.");

        font.AddCommand<FontInspectCommand>("inspect")
            .WithDescription("Reports an .xfnt: v1 geometry and slots (--show a glyph), v2 metrics and counts (--text draws a line in the terminal through the oracle rasteriser).");

        font.AddCommand<FontRasterCommand>("raster")
            .WithDescription("Draws every glyph of v2 fonts through the oracle rasteriser, a line each with a hash of its coverage: the C# side of the Differential fact that holds std/raster.xfxn to it (os/xc/rastdump.xfxn prints the same).");
    });

    // ---- pictures: the desktop's background ----
    config.AddBranch("picture", picture =>
    {
        picture.SetDescription("Converts JPEG pictures into the QOI the OS reads, and strips their metadata.");

        picture.AddCommand<PictureBuildCommand>("build")
            .WithDescription("Decodes a baseline JPEG, scales it down to cover a screen (--cover WxH) and writes QOI. Refuses a source still carrying metadata.");
        picture.AddCommand<PictureStripCommand>("strip")
            .WithDescription("Removes EXIF (with any GPS position), XMP, IPTC and comments from JPEGs in place, copying the image data unchanged.");
    });

    // ---- localisation (roadmap 2.8a4, system/CX_TOOLKIT.md §10) ----
    config.AddBranch("strings", strings =>
    {
        strings.SetDescription("Builds XSTR string tables from Fluent sources, with CLDR's plural rules.");

        strings.AddCommand<StringsBuildCommand>("build")
            .WithDescription("Compiles an app's .ftl sources into one locale's .xstr; --base holds a translation to the English, --pseudo writes the pseudo-locale.");
        strings.AddCommand<StringsPluralsCommand>("plurals")
            .WithDescription("Writes every CLDR locale's plural rules with CLDR's samples (.xplt), for the boot test.");
    });
    config.AddBranch("unicode", unicode =>
    {
        unicode.SetDescription("Generates tables from the Unicode Character Database.");

        unicode.AddCommand<UnicodeBidiCommand>("bidi")
            .WithDescription("Writes os/std/unibidi.xfxn, the tables of the UAX #9 implementation; --check compares instead.");
    });

    // ---- the OS build (replaces tools/build.bat) ----
    config.AddBranch("os", os =>
    {
        os.SetDescription("Builds the OS: boot chain, kernel, executive, X userland and the disk image.");

        os.AddCommand<OsBuildCommand>("build")
            .WithDescription("Builds CXK with clang, ld.lld and Ninja (replaces tools/build.bat).");
    });

    // ---- UEFI boot stub (replaces boot/uefi/build.bat and its MSVC prompt) ----
    config.AddBranch("uefi", uefi =>
    {
        uefi.SetDescription("Builds the UEFI boot stub.");

        uefi.AddCommand<UefiBuildCommand>("build")
            .WithDescription("Builds BOOTX64.EFI with clang and lld-link, on any host (replaces boot/uefi/build.bat).");
    });

    // ---- UEFI Secure Boot (replaces openssl + virt-fw-vars + sbsign + sbverify) ----
    config.AddBranch("secureboot", sb =>
    {
        sb.SetDescription("Owns a platform's Secure Boot keys, and signs EFI binaries with them.");

        sb.AddCommand<SecureBootKeygenCommand>("keygen")
            .WithDescription("Generates a PK/KEK/db key set (.pfx private, .cer/.pem public).");

        sb.AddCommand<SecureBootVarStoreCommand>("varstore")
            .WithDescription("Enrolls a key set into an OVMF variable store and turns Secure Boot on.");

        sb.AddCommand<SecureBootSignCommand>("sign")
            .WithDescription("Authenticode-signs a PE image so Secure Boot firmware will load it.");

        sb.AddCommand<SecureBootVerifyCommand>("verify")
            .WithDescription("Checks whether firmware holding a given certificate would accept an image.");

        sb.AddCommand<SecureBootTestCommand>("test")
            .WithDescription("Boots a stub under enforced Secure Boot, signed and unsigned, and checks both.");
    });
});

// Spectre wraps every line it writes to the profile width, and under a
// redirected stdout with TERM=linux - the default on a Linux virtual console
// and in most CI containers - System.Console.WindowWidth reports -1 rather
// than 0. Spectre guards against 0 but not against a negative, so the width
// stays -1 and every line wraps to nothing: the tool produces NO output at
// all, on either stream, while still exiting with the right code.
//
// That turns a compile error into a bare "Error 1" from the build system with
// nothing to act on, which is worse than useless - it looks like the toolchain
// is broken rather than the source. Clamp the width to something sane whenever
// detection fails, before any command runs.
if (AnsiConsole.Profile.Width <= 0)
{
    AnsiConsole.Profile.Width = 100;
    AnsiConsole.Profile.Height = 40;
}

// Run the application
try
{
    return app.Run(args);
}
catch (Exception ex)
{
    AnsiConsole.MarkupLine($"[red]Fatal Error:[/] {ex.Message}");
    return 1;
}
