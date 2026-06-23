using System.ComponentModel;
using System.Threading;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

public class SignCommand : Command<SignCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<TARGET_FILE>")]
        public string TargetPath { get; set; } = string.Empty;

        [CommandArgument(1, "<PRIVATE_KEY>")]
        public string PrivateKeyPath { get; set; } = string.Empty;

        [CommandArgument(2, "<PUBLIC_KEY>")]
        public string PublicKeyPath { get; set; } = string.Empty;
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        return 0; // We will plug in CXEX.Crypto next!
    }
}