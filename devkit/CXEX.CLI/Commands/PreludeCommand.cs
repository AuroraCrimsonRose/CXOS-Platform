// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading;
using CXEX.Lang.Abi;
using CXEX.Lang.Ast;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Print the ABI prelude - the declarations `cxk compile` puts in front of
/// every program unless told --no-prelude. The X compiler written in X reads
/// it from a file, so the build writes it out with this.
/// </summary>
public class PreludeCommand : Command<PreludeCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "[OUT]")]
        [Description("write it to this file instead of stdout")]
        public string? Out { get; set; }

        [CommandOption("--target <TARGET>")]
        [Description("x86_64 (the default) or x86_32: the syscall wrappers pass registers as that machine's are")]
        public string Target { get; set; } = "x86_64";
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        var target = XTarget.Parse(settings.Target);
        if (target == null)
        {
            Console.Error.WriteLine($"error: unknown target '{settings.Target}' (x86_32 or x86_64)");
            return 1;
        }
        var bytes = Encoding.UTF8.GetBytes(AbiPrelude.Generate(target));
        if (settings.Out != null) { File.WriteAllBytes(settings.Out, bytes); return 0; }
        using var stdout = Console.OpenStandardOutput();
        stdout.Write(bytes, 0, bytes.Length);
        return 0;
    }
}
