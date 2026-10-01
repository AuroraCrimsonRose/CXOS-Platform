using System;
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
    public class Settings : CommandSettings { }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        using var stdout = Console.OpenStandardOutput();
        var bytes = Encoding.UTF8.GetBytes(AbiPrelude.Generate());
        stdout.Write(bytes, 0, bytes.Length);
        return 0;
    }
}
