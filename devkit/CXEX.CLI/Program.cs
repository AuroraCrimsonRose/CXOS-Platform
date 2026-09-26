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
        .WithDescription("Compiles an ELF binary into a CXEX executable (.xkex, .xoex, .xcex).");

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
