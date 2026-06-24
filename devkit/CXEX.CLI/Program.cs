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

    // X language compiler (CXEX.Lang): .x source -> ELF
    config.AddCommand<CompileCommand>("compile")
        .WithDescription("Compiles an X source file (.x) into an ELF, ready for `build` to package as CXEX.");

    // 1. The Compiler (Replaces mkcxes.py)
    config.AddCommand<BuildCommand>("build")
        .WithDescription("Compiles an ELF binary into a CXEX executable (.xkex, .xoex, .xcex).");

    config.AddCommand<SignCommand>("sign")
        .WithDescription("Appends a CXSG cryptographic signature block to a CXEX image.");

    config.AddCommand<ImageCommand>("image")
        .WithDescription("Compiles stage1, stage2, the kernel, and the CXFS payload into a bootable XBPT disk image.");

    config.AddCommand<EmbedCommand>("embed")
        .WithDescription("Converts a binary file into a C header byte array.");

    config.AddCommand<CheckCommand>("check")
        .WithDescription("Validates that all source files listed in CMakeLists.txt exist.");

    config.AddCommand<RawImageCommand>("raw-image")
        .WithDescription("Creates a flat, padded raw binary disk (replaces pad_boot.ps1).");
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