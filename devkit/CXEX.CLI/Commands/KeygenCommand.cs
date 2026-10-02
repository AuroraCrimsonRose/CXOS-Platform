// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Threading;
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.Crypto.Trust;

namespace CXEX.CLI.Commands;

public class KeygenCommand : Command<KeygenCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<NAME>")]
        [Description("Base name for the keypair; writes <NAME>.xksk and <NAME>.xkpk")]
        public string Name { get; set; } = string.Empty;

        [CommandOption("-b|--bits")]
        [Description("RSA key size in bits")]
        [DefaultValue(2048)]
        public int Bits { get; set; } = 2048;

        [CommandOption("-f|--force")]
        [Description("Overwrite an existing keypair")]
        public bool Force { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        string sk = settings.Name + ".xksk";
        string pk = settings.Name + ".xkpk";

        // Silently clobbering a signing key would be unrecoverable: every image
        // ever signed with it stops verifying, and the embedded trusted_key.c no
        // longer matches. Make the user say so explicitly.
        if (!settings.Force && (File.Exists(sk) || File.Exists(pk)))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{sk}' or '{pk}' already exists. Use --force to overwrite.");
            AnsiConsole.MarkupLine("[yellow]Warning:[/] overwriting a signing key invalidates every image signed with it.");
            return 1;
        }

        byte[] xkpk;
        try
        {
            xkpk = CXKeyGenerator.Generate(sk, pk, settings.Bits);
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] {ex.Message}");
            return 1;
        }

        byte[] fp = CXKeyGenerator.Fingerprint(xkpk);

        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] Generated {settings.Bits}-bit keypair");
        AnsiConsole.MarkupLine($"  private key  [cyan]{sk}[/] [red](never commit or ship this)[/]");
        AnsiConsole.MarkupLine($"  public key   [cyan]{pk}[/] ({xkpk.Length} bytes)");
        AnsiConsole.MarkupLine($"  fingerprint  [grey]{Convert.ToHexString(fp).ToLowerInvariant()}[/]");
        AnsiConsole.MarkupLine("");
        // No `cxk embed` step any more: the build generates the kernel's root of
        // trust from the public half of whichever key it signs with, so telling
        // anyone to embed one by hand is telling them how to produce a kernel
        // that trusts a different key than the one signing its userland.
        string baseName = Path.GetFileNameWithoutExtension(sk);
        AnsiConsole.MarkupLine($"Next: [grey]cxk os build --key {baseName}[/] - the build signs with this pair and compiles the public half in as the kernel's root of trust.");

        return 0;
    }
}
