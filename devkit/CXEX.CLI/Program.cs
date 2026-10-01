using System;
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.CLI.Commands;

// Initialize the Spectre Command App
var app = new CommandApp();

app.Configure(config =>
{
    config.SetApplicationName("cxk");
    config.SetApplicationVersion("5.0.0");

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

    config.AddCommand<RawImageCommand>("raw-image")
        .WithDescription("Creates a flat, padded raw binary disk (replaces pad_boot.ps1).");

    // ---- run + inspect ----
    config.AddCommand<RunCommand>("run")
        .WithDescription("Boots a CXK disk image in QEMU or Bochs (replaces run_qemu.bat / run_bochs.bat).");

    config.AddCommand<InspectCommand>("inspect")
        .WithDescription("Dumps a CXEX image's header, sections, and signature (replaces cxkdump.py).");

    // ---- validation ----
    config.AddCommand<CheckCommand>("check")
        .WithDescription("Validates that all source files listed in CMakeLists.txt exist.");

    config.AddCommand<CheckAbiCommand>("check-abi")
        .WithDescription("Validates that the X ABI prelude still matches the kernel's cxk_abi.h.");

    config.AddCommand<TokensCommand>("tokens")
        .WithDescription("Prints the tokens of X source files (compared against the lexer written in X).");

    config.AddCommand<AstCommand>("ast")
        .WithDescription("Prints the syntax trees of X source files (compared against the parser written in X).");

    config.AddCommand<SemaCommand>("sema")
        .WithDescription("Prints each expression's type and every diagnostic for X programs (compared against the type checker written in X).");

    config.AddCommand<AsmCommand>("asm")
        .WithDescription("Prints the assembly `compile` generates for X programs (compared against the code generator written in X).");

    config.AddCommand<PreludeCommand>("prelude")
        .WithDescription("Prints the ABI prelude that `compile` puts in front of every program.");

    config.AddCommand<CheckXDataCommand>("check-xdata")
        .WithDescription("Validates X Data documents (service descriptors), with the same rules as the X reader.");

    // ---- the OS build (replaces tools/build.bat) ----
    config.AddBranch("os", os =>
    {
        os.SetDescription("Builds the OS: boot chain, kernel, executive, X userland and the disk image.");

        os.AddCommand<OsBuildCommand>("build")
            .WithDescription("Builds CXK with clang, ld.lld and Ninja (replaces tools/build.bat).");
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
