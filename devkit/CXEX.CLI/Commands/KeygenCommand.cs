using System.Threading;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

public class KeygenCommand : Command<KeygenCommand.Settings>
{
    public class Settings : CommandSettings { }
    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken) => 0;
}