using System;
using System.ComponentModel;
using System.IO;
using System.Text;
using System.Threading;
using CXEX.Lang.Abi;
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
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(AbiPrelude.Generate());
        if (settings.Out != null) { File.WriteAllBytes(settings.Out, bytes); return 0; }
        using var stdout = Console.OpenStandardOutput();
        stdout.Write(bytes, 0, bytes.Length);
        return 0;
    }
}
