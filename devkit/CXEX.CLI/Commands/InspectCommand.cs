using System.Threading;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

public class InspectCommand : Command<InspectCommand.Settings>
{
    public class Settings : CommandSettings { }
    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken) => 0;
}