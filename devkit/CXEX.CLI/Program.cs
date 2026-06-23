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

    // 1. The Compiler (Replaces mkcxes.py)
    config.AddCommand<BuildCommand>("build")
        .WithDescription("Compiles an ELF binary into a CXEX executable (.xkex, .xoex, .xcex).");

    // 2. The Cryptographer (Replaces signcxex.py) - We will build this next!
    // config.AddCommand<SignCommand>("sign")
    //     .WithDescription("Appends a CXSG cryptographic signature block to a CXEX image.");

    // 3. The Disk Stager (Replaces mkdisk.py & pad_disk.ps1) - We will build this next!
    // config.AddCommand<ImageCommand>("image")
    //     .WithDescription("Compiles stage1, stage2, the kernel, and the CXFS payload into a bootable XBPT disk image.");
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