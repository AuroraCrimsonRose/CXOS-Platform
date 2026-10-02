// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Threading;
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.Crypto.Signing;
using CXEX.Crypto.Trust;

namespace CXEX.CLI.Commands;

public class SignCommand : Command<SignCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<TARGET_FILE>")]
        [Description("CXEX image to sign in place (.xkex / .xoex / .xsex / .xuex)")]
        public string TargetPath { get; set; } = string.Empty;

        [CommandArgument(1, "<PRIVATE_KEY>")]
        [Description("PEM private signing key (.xksk)")]
        public string PrivateKeyPath { get; set; } = string.Empty;

        [CommandArgument(2, "<PUBLIC_KEY>")]
        [Description("Matching public key (.xkpk); its sha256 becomes the CXSG fingerprint")]
        public string PublicKeyPath { get; set; } = string.Empty;
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        foreach (var (label, path) in new[]
                 {
                     ("Target", settings.TargetPath),
                     ("Private key", settings.PrivateKeyPath),
                     ("Public key", settings.PublicKeyPath)
                 })
        {
            if (!File.Exists(path))
            {
                AnsiConsole.MarkupLine($"[red]Error:[/] {label} '{path}' not found.");
                return 1;
            }
        }

        try
        {
            CXSigner.SignArtifact(settings.TargetPath, settings.PrivateKeyPath, settings.PublicKeyPath);
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] {ex.Message}");
            return 1;
        }

        byte[] fp = CXKeyGenerator.Fingerprint(File.ReadAllBytes(settings.PublicKeyPath));

        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] Signed [cyan]{settings.TargetPath}[/]");
        AnsiConsole.MarkupLine($"  fingerprint  [grey]{Convert.ToHexString(fp).ToLowerInvariant()}[/]");
        AnsiConsole.MarkupLine("[grey]  (must match the key embedded via `embed` or the kernel rejects it: CXEX_VERIFY_WRONG_KEY)[/]");

        return 0;
    }
}
