using System.ComponentModel;
using System.Diagnostics;
using System.Text.RegularExpressions;
using CXEX.Uefi.Authenticode;
using CXEX.Uefi.SecureBoot;
using Spectre.Console;
using Spectre.Console.Cli;

namespace CXEX.CLI.Commands;

/// <summary>
/// <c>cxk secureboot test</c> - boots a stub under enforced Secure Boot, twice.
/// </summary>
/// <remarks>
/// <para>
/// The two runs are not redundant. The signed run shows the stub loads and reports
/// Secure Boot on. The unsigned run shows firmware would have refused it - and
/// without that, a misconfigured setup looks exactly like a pass, because OVMF
/// built without SMM support boots unsigned binaries while still reporting
/// whatever the SecureBoot variable says.
/// </para>
/// <para>
/// Hence <c>smm=on</c> together with <c>secure=on</c> on the flash device. Neither
/// alone enforces anything: without SMM the variable store is writable from
/// outside SMRAM, so firmware does not treat it as authoritative.
/// </para>
/// </remarks>
public class SecureBootTestCommand : Command<SecureBootTestCommand.Settings>
{
    public class Settings : SecureBootKeySettings
    {
        [CommandArgument(0, "[STUB]")]
        [Description("The unsigned EFI application to test")]
        [DefaultValue("BOOTX64.EFI")]
        public string Stub { get; set; } = "BOOTX64.EFI";

        [CommandOption("--firmware-dir")]
        [Description("Directory to search for OVMF firmware")]
        public string? FirmwareDir { get; set; }

        [CommandOption("--vars")]
        [Description("Enrolled variable store; defaults to <keys>/CXK_VARS.fd")]
        public string? Vars { get; set; }

        [CommandOption("--timeout")]
        [Description("Seconds to let each boot run")]
        [DefaultValue(45)]
        public int Timeout { get; set; } = 45;

        [CommandOption("--keep")]
        [Description("Keep the scratch directory instead of deleting it")]
        public bool Keep { get; set; }
    }

    protected override int Execute(CommandContext context, Settings settings, CancellationToken ct)
    {
        string? qemu = UefiFirmware.FindQemu();
        if (qemu is null)
        {
            AnsiConsole.MarkupLine("[red]Error:[/] qemu-system-x86_64 not found on PATH or in the usual install locations.");
            return 1;
        }
        string? code = UefiFirmware.FindCode(settings.FirmwareDir);
        if (code is null)
        {
            AnsiConsole.MarkupLine("[red]Error:[/] no Secure-Boot-capable OVMF CODE image found.");
            UefiFirmware.ReportSearch(settings.FirmwareDir);
            return 1;
        }
        string vars = settings.Vars ?? settings.Path("CXK_VARS.fd");
        if (!File.Exists(vars))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{vars}' not found. Run [grey]cxk secureboot varstore[/] first.");
            return 1;
        }
        if (!File.Exists(settings.Stub))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{settings.Stub}' not found. Build the stub first.");
            return 1;
        }

        // Refuse a store that does not actually enable Secure Boot, rather than
        // running two boots and reporting a confusing failure.
        try
        {
            var store = EfiVarStore.Load(vars);
            var sbe = store.Get("SecureBootEnable", EfiGuids.SecureBootEnableDisable);
            var pk = store.Get("PK", EfiGuids.GlobalVariable);
            if (pk is null || sbe is null || sbe.Data.Length < 1 || sbe.Data[0] == 0)
            {
                AnsiConsole.MarkupLine($"[red]Error:[/] '{vars}' has no PK enrolled or Secure Boot is not enabled.");
                return 1;
            }
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{vars}' is not a usable variable store: {ex.Message}");
            return 1;
        }

        string work = Path.Combine(Path.GetTempPath(), "cxk-sbtest-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            string signedStub = Path.Combine(work, "BOOTX64.signed.efi");
            Directory.CreateDirectory(work);

            using (var cert = SecureBootKeys.LoadPfx(settings.Path("db.pfx"), settings.Password))
            {
                var pe = PeImage.Load(settings.Stub);
                File.WriteAllBytes(signedStub, AuthenticodeSigner.Sign(pe, cert));
                if (!AuthenticodeSigner.Verify(PeImage.Load(signedStub), cert, out string why))
                {
                    AnsiConsole.MarkupLine($"[red]Error:[/] the signature did not verify before booting: {why}");
                    return 1;
                }
            }

            AnsiConsole.MarkupLine($"[grey]qemu    {qemu}[/]");
            AnsiConsole.MarkupLine($"[grey]code    {code}[/]");
            AnsiConsole.MarkupLine($"[grey]vars    {vars}[/]");
            AnsiConsole.MarkupLine("");

            AnsiConsole.MarkupLine("[bold]signed stub[/] - expect it to run and report Secure Boot on");
            string signedLog = RunOne(qemu, code, vars, signedStub, work, "signed", settings.Timeout);
            Print(signedLog);

            AnsiConsole.MarkupLine("");
            AnsiConsole.MarkupLine("[bold]unsigned stub[/] - expect firmware to refuse it");
            string unsignedLog = RunOne(qemu, code, vars, settings.Stub, work, "unsigned", settings.Timeout);
            Print(unsignedLog);

            bool ranSigned = signedLog.Contains("Secure Boot", StringComparison.OrdinalIgnoreCase)
                             && !signedLog.Contains("Access Denied", StringComparison.OrdinalIgnoreCase);
            bool refusedUnsigned = unsignedLog.Contains("Access Denied", StringComparison.OrdinalIgnoreCase);

            AnsiConsole.MarkupLine("");
            AnsiConsole.MarkupLine(ranSigned
                ? "  [green]PASS[/] the signed stub was loaded"
                : "  [red]FAIL[/] the signed stub did not run - firmware rejected our own signature");
            AnsiConsole.MarkupLine(refusedUnsigned
                ? "  [green]PASS[/] the unsigned stub was refused, so enforcement is real"
                : "  [red]FAIL[/] the unsigned stub was NOT refused");

            if (!refusedUnsigned)
            {
                AnsiConsole.MarkupLine("");
                AnsiConsole.MarkupLine("[yellow]The unsigned stub booted, which means Secure Boot is not being enforced[/]");
                AnsiConsole.MarkupLine("[yellow]and the signed run proves nothing. Almost always one of:[/]");
                AnsiConsole.MarkupLine($"  - [grey]{Path.GetFileName(code)}[/] is not a Secure-Boot-capable build");
                AnsiConsole.MarkupLine("  - this QEMU does not support SMM, so the variable store is not authoritative");
            }

            return ranSigned && refusedUnsigned ? 0 : 1;
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] {ex.Message}");
            return 1;
        }
        finally
        {
            if (settings.Keep) AnsiConsole.MarkupLine($"[grey]scratch kept at {work}[/]");
            else try { Directory.Delete(work, true); } catch (IOException) { /* scratch, not worth failing over */ }
        }
    }

    private static string RunOne(string qemu, string code, string varsTemplate, string stub,
                                 string work, string tag, int timeoutSeconds)
    {
        string esp = Path.Combine(work, "esp-" + tag);
        Directory.CreateDirectory(Path.Combine(esp, "EFI", "BOOT"));
        File.Copy(stub, Path.Combine(esp, "EFI", "BOOT", "BOOTX64.EFI"), true);

        // Each run gets its own copy: firmware writes to the store, and a run that
        // modified it would change what the next run is testing.
        string vars = Path.Combine(work, $"vars-{tag}.fd");
        File.Copy(varsTemplate, vars, true);
        string log = Path.Combine(work, $"{tag}.log");

        // The ESP is passed as a path relative to the working directory. QEMU's
        // vvfat option is colon-delimited, so an absolute Windows path would put a
        // drive-letter colon inside "file=fat:rw:..." and be mis-parsed.
        var args = new List<string>
        {
            "-machine", "q35,smm=on",
            "-m", "512",
            "-global", "driver=cfi.pflash01,property=secure,value=on",
            "-global", "ICH9-LPC.disable_s3=1",
            "-drive", $"if=pflash,format=raw,unit=0,readonly=on,file={code}",
            "-drive", $"if=pflash,format=raw,unit=1,file={vars}",
            "-drive", $"file=fat:rw:{Path.GetFileName(esp)},format=raw",
            "-net", "none",
            "-display", "none",
            "-serial", $"file:{log}",
        };

        var psi = new ProcessStartInfo(qemu) { WorkingDirectory = work, UseShellExecute = false };
        foreach (var a in args) psi.ArgumentList.Add(a);

        using var p = Process.Start(psi) ?? throw new Exception("could not start QEMU");
        // The stub does not call ExitBootServices, so it never returns and QEMU
        // never exits on its own. Killing it after the timeout is the normal path,
        // not an error.
        if (!p.WaitForExit(timeoutSeconds * 1000))
        {
            try { p.Kill(true); } catch (InvalidOperationException) { }
            p.WaitForExit(5000);
        }

        for (int i = 0; i < 10 && !File.Exists(log); i++) Thread.Sleep(200);
        if (!File.Exists(log)) return "";
        using var fs = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using var sr = new StreamReader(fs);
        return Strip(sr.ReadToEnd());
    }

    // The UEFI console drives a terminal: cursor moves, clears, mode sets. Left in,
    // they make the log unreadable and unsearchable.
    private static readonly Regex AnsiEscape = new(@"\x1b\[[0-9;=?]*[A-Za-z]", RegexOptions.Compiled);

    private static string Strip(string s) => AnsiEscape.Replace(s.Replace("\r", ""), "");

    private static void Print(string log)
    {
        var lines = log.Split('\n').Select(l => l.TrimEnd()).Where(l => l.Length > 0).ToArray();
        if (lines.Length == 0)
        {
            AnsiConsole.MarkupLine("  [yellow](no serial output)[/]");
            return;
        }
        foreach (var l in lines.TakeLast(20)) AnsiConsole.MarkupLine($"  [grey]{Markup.Escape(l)}[/]");
    }
}
