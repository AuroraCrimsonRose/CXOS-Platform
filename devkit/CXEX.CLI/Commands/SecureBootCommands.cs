using System.ComponentModel;
using System.Security.Cryptography.X509Certificates;
using CXEX.Uefi.Authenticode;
using CXEX.Uefi.SecureBoot;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// Shared settings for the secureboot commands: where the key set lives and how
/// the PFX is protected.
/// </summary>
public class SecureBootKeySettings : CommandSettings
{
    [CommandOption("-k|--keys")]
    [Description("Directory holding the key set")]
    [DefaultValue("sbkeys")]
    public string Keys { get; set; } = "sbkeys";

    [CommandOption("-p|--password")]
    [Description("Password protecting the .pfx files")]
    [DefaultValue("cxk")]
    public string Password { get; set; } = "cxk";

    public string Path(string name) => System.IO.Path.Combine(Keys, name);
}

/// <summary>
/// <c>cxk secureboot keygen</c> - creates the PK/KEK/db set used to own a platform.
/// </summary>
public class SecureBootKeygenCommand : Command<SecureBootKeygenCommand.Settings>
{
    public class Settings : SecureBootKeySettings
    {
        [CommandOption("-o|--org")]
        [Description("Organisation name placed in each certificate's subject")]
        [DefaultValue("CATX Systems")]
        public string Organisation { get; set; } = "CATX Systems";

        [CommandOption("-b|--bits")]
        [Description("RSA key size in bits")]
        [DefaultValue(2048)]
        public int Bits { get; set; } = 2048;

        [CommandOption("-f|--force")]
        [Description("Overwrite an existing key set")]
        public bool Force { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken ct)
    {
        Directory.CreateDirectory(settings.Keys);

        // Regenerating silently would invalidate any variable store already built
        // from the old keys, and on real hardware would orphan an enrolled PK -
        // leaving a machine that trusts a key nobody holds any more.
        if (!settings.Force && File.Exists(settings.Path("db.pfx")))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] a key set already exists in '{settings.Keys}'. Use --force to replace it.");
            AnsiConsole.MarkupLine("[yellow]Warning:[/] replacing these keys invalidates every variable store and signature made with them.");
            return 1;
        }

        var owner = Guid.NewGuid();
        File.WriteAllText(settings.Path("owner.guid"), owner.ToString());

        foreach (var role in new[] { SecureBootKeys.Role.Pk, SecureBootKeys.Role.Kek, SecureBootKeys.Role.Db })
        {
            using var cert = SecureBootKeys.Create(role, settings.Organisation, settings.Bits);
            string name = role.ToString().ToLowerInvariant() switch { "pk" => "PK", "kek" => "KEK", _ => "db" };
            SecureBootKeys.Export(cert, settings.Path(name), settings.Password);
            AnsiConsole.MarkupLine($"  [cyan]{name,-3}[/] {cert.Subject}  [grey]{cert.Thumbprint[..16].ToLowerInvariant()}[/]");
        }

        AnsiConsole.MarkupLine($"[green]SUCCESS:[/] wrote a {settings.Bits}-bit key set to [cyan]{settings.Keys}[/]");
        AnsiConsole.MarkupLine($"  owner GUID  [grey]{owner}[/]");
        AnsiConsole.MarkupLine("  [red].pfx and .pem hold PRIVATE keys - never commit them[/]");
        AnsiConsole.MarkupLine("  .cer is DER, which is what firmware setup menus accept for manual enrolment");
        AnsiConsole.MarkupLine("");
        AnsiConsole.MarkupLine("Next: [grey]cxk secureboot varstore[/] to build a variable store with these enrolled.");
        return 0;
    }
}

/// <summary>
/// <c>cxk secureboot varstore</c> - enrolls a key set into an OVMF variable store.
/// </summary>
public class SecureBootVarStoreCommand : Command<SecureBootVarStoreCommand.Settings>
{
    public class Settings : SecureBootKeySettings
    {
        [CommandOption("-t|--template")]
        [Description("Stock OVMF vars image to start from; found automatically if omitted")]
        public string? Template { get; set; }

        [CommandOption("-O|--out")]
        [Description("Output path for the enrolled store")]
        public string? Output { get; set; }

        [CommandOption("--firmware-dir")]
        [Description("Directory to search for OVMF firmware")]
        public string? FirmwareDir { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken ct)
    {
        string? template = settings.Template ?? UefiFirmware.FindVarsTemplate(settings.FirmwareDir);
        if (template is null)
        {
            AnsiConsole.MarkupLine("[red]Error:[/] no OVMF vars template found.");
            UefiFirmware.ReportSearch(settings.FirmwareDir);
            return 1;
        }

        string output = settings.Output ?? settings.Path("CXK_VARS.fd");
        if (!File.Exists(settings.Path("db.pfx")))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] no key set in '{settings.Keys}'. Run [grey]cxk secureboot keygen[/] first.");
            return 1;
        }

        Guid owner = File.Exists(settings.Path("owner.guid"))
            ? Guid.Parse(File.ReadAllText(settings.Path("owner.guid")).Trim())
            : Guid.NewGuid();

        try
        {
            var store = EfiVarStore.Load(template);
            using var pk = SecureBootKeys.LoadPfx(settings.Path("PK.pfx"), settings.Password);
            using var kek = SecureBootKeys.LoadPfx(settings.Path("KEK.pfx"), settings.Password);
            using var db = SecureBootKeys.LoadPfx(settings.Path("db.pfx"), settings.Password);

            SecureBootKeys.Enroll(store, pk, kek, db, owner);
            store.Save(output);

            AnsiConsole.MarkupLine($"[green]SUCCESS:[/] enrolled into [cyan]{output}[/]");
            AnsiConsole.MarkupLine($"  template    [grey]{template}[/]");
            foreach (var v in store.Variables.OrderBy(v => v.Name))
                AnsiConsole.MarkupLine($"  {v.Name,-17} {v.Data.Length,6} bytes  attrs 0x{v.Attributes:X2}");
            AnsiConsole.MarkupLine($"  free space  {store.FreeSpace()} bytes");
            AnsiConsole.MarkupLine("");
            AnsiConsole.MarkupLine("[grey]Microsoft's keys are deliberately not enrolled: with them present, a failure[/]");
            AnsiConsole.MarkupLine("[grey]to verify our own signature could be masked by anything else MS-signed.[/]");
            return 0;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] {ex.Message}");
            return 1;
        }
    }
}

/// <summary>
/// <c>cxk secureboot sign</c> - Authenticode-signs a PE so firmware will load it.
/// </summary>
public class SecureBootSignCommand : Command<SecureBootSignCommand.Settings>
{
    public class Settings : SecureBootKeySettings
    {
        [CommandArgument(0, "<INPUT>")]
        [Description("PE image to sign (BOOTX64.EFI)")]
        public string Input { get; set; } = string.Empty;

        [CommandArgument(1, "[OUTPUT]")]
        [Description("Where to write the signed image; defaults to overwriting the input")]
        public string? Output { get; set; }

        [CommandOption("--pfx")]
        [Description("Signing certificate; defaults to <keys>/db.pfx")]
        public string? Pfx { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken ct)
    {
        string pfx = settings.Pfx ?? settings.Path("db.pfx");
        string output = settings.Output ?? settings.Input;

        if (!File.Exists(settings.Input))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{settings.Input}' not found.");
            return 1;
        }
        if (!File.Exists(pfx))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] signing certificate '{pfx}' not found. Run [grey]cxk secureboot keygen[/] first.");
            return 1;
        }

        try
        {
            var pe = PeImage.Load(settings.Input);
            using var cert = SecureBootKeys.LoadPfx(pfx, settings.Password);
            byte[] signed = AuthenticodeSigner.Sign(pe, cert);
            File.WriteAllBytes(output, signed);

            // Read it back and verify rather than trusting that writing worked. A
            // signature that does not verify here will simply be refused by
            // firmware with "Access Denied" and nothing to debug from.
            var check = PeImage.Load(output);
            if (!AuthenticodeSigner.Verify(check, cert, out string why))
            {
                AnsiConsole.MarkupLine($"[red]Error:[/] the signature did not verify after writing: {why}");
                return 1;
            }

            AnsiConsole.MarkupLine($"[green]SUCCESS:[/] signed [cyan]{output}[/]");
            AnsiConsole.MarkupLine($"  image hash  [grey]{Convert.ToHexString(pe.ComputeHash(System.Security.Cryptography.HashAlgorithmName.SHA256)).ToLowerInvariant()}[/]");
            AnsiConsole.MarkupLine($"  signer      {cert.Subject}");
            AnsiConsole.MarkupLine($"  size        {pe.Bytes.Length} -> {signed.Length} bytes");
            return 0;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] {ex.Message}");
            return 1;
        }
    }
}

/// <summary>
/// <c>cxk secureboot verify</c> - would firmware holding this certificate accept this file.
/// </summary>
public class SecureBootVerifyCommand : Command<SecureBootVerifyCommand.Settings>
{
    public class Settings : SecureBootKeySettings
    {
        [CommandArgument(0, "<INPUT>")]
        [Description("Signed PE image to check")]
        public string Input { get; set; } = string.Empty;

        [CommandOption("-c|--cert")]
        [Description("Certificate to check against; defaults to <keys>/db.cer")]
        public string? Cert { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken ct)
    {
        string certPath = settings.Cert ?? settings.Path("db.cer");
        if (!File.Exists(settings.Input))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{settings.Input}' not found.");
            return 1;
        }
        if (!File.Exists(certPath))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] certificate '{certPath}' not found.");
            return 1;
        }

        try
        {
            var pe = PeImage.Load(settings.Input);
            using var cert = SecureBootKeys.LoadCertificate(certPath);
            if (AuthenticodeSigner.Verify(pe, cert, out string why))
            {
                AnsiConsole.MarkupLine($"[green]OK:[/] '{settings.Input}' verifies against {cert.Subject}");
                return 0;
            }
            AnsiConsole.MarkupLine($"[red]FAILED:[/] {why}");
            return 1;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] {ex.Message}");
            return 1;
        }
    }
}
