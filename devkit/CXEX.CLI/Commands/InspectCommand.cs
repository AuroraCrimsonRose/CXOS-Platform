using System;
using System.ComponentModel;
using System.IO;
using System.Threading;
using Spectre.Console;
using Spectre.Console.Cli;
using CXEX.FileType.Parsers;

namespace CXEX.CLI.Commands;

/// <summary>
/// Dumps a CXEX image's header, section table, and signature block. The X-toolchain
/// debugging companion: build the same program in C and in X, `inspect` both, and
/// the field-by-field diff shows immediately if the emitters disagree on layout.
/// Replaces cxkdump.py.
/// </summary>
public class InspectCommand : Command<InspectCommand.Settings>
{
    public class Settings : CommandSettings
    {
        [CommandArgument(0, "<IMAGE>")]
        [Description("CXEX image to inspect (.xkex / .xoex / .xcex)")]
        public string Path { get; set; } = string.Empty;
    }

    private static string TypeName(ushort t) => t switch
    {
        0x4B45 => "KERNEL",
        0x4245 => "BOOT",
        0x4F45 => "OS",
        0x4345 => "USER",
        _ => "?"
    };

    protected override int Execute(CommandContext context, Settings settings, CancellationToken cancellationToken)
    {
        if (!File.Exists(settings.Path))
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] '{settings.Path}' not found.");
            return 1;
        }

        byte[] data = File.ReadAllBytes(settings.Path);

        try
        {
            var h = CXEXParser.ParseHeader(data);

            AnsiConsole.MarkupLine($"[bold]{System.IO.Path.GetFileName(settings.Path)}[/]  ({data.Length} bytes)");
            AnsiConsole.MarkupLine($"  type          [cyan]{TypeName(h.TypeCode)}[/] (0x{h.TypeCode:X4})");
            AnsiConsole.MarkupLine($"  format_ver    {h.FormatVersion}");
            AnsiConsole.MarkupLine($"  arch          {h.ArchTarget}");
            AnsiConsole.MarkupLine($"  abi_ver       {h.AbiVersion}");
            AnsiConsole.MarkupLine($"  flags         0x{h.Flags:X2}   [grey]{DescribeFlags(h.Flags)}[/]");
            AnsiConsole.MarkupLine($"  entry         0x{h.EntryPoint:X8}");
            AnsiConsole.MarkupLine($"  load_base     0x{h.LoadBase:X8}");
            AnsiConsole.MarkupLine($"  phys_base     0x{ReadPhysBase(data):X8}   [grey](offset 48; used by cxexload.asm)[/]");
            AnsiConsole.MarkupLine($"  image_min/max 0x{h.ImageMin:X8} .. 0x{h.ImageMax:X8}");
            AnsiConsole.MarkupLine($"  sections      {h.SectionCount} @ {h.SectionOffset}");
            AnsiConsole.MarkupLine($"  sig_offset    {(h.SignatureOffset == 0 ? "(unsigned)" : $"{h.SignatureOffset}")}");

            if (h.SectionCount > 0)
            {
                AnsiConsole.MarkupLine("");
                var table = new Table().Border(TableBorder.Rounded);
                table.AddColumn("name");
                table.AddColumn(new TableColumn("file_off").RightAligned());
                table.AddColumn(new TableColumn("virt_addr").RightAligned());
                table.AddColumn(new TableColumn("file_sz").RightAligned());
                table.AddColumn(new TableColumn("mem_sz").RightAligned());
                table.AddColumn(new TableColumn("flags").RightAligned());

                int off = h.SectionOffset;
                for (int i = 0; i < h.SectionCount; i++)
                {
                    var s = CXEXParser.ParseSection(data, off);
                    table.AddRow(
                        s.Name,
                        s.FileOffset.ToString(),
                        $"0x{s.VirtAddr:X8}",
                        s.FileSize.ToString(),
                        s.MemSize.ToString(),
                        DescribeSecFlags(s.Flags));
                    off += CXEXParser.SECTION_SIZE;
                }
                AnsiConsole.Write(table);
            }

            // Signed? The flag (bit 2) and a nonzero offset should agree.
            bool flagSigned = (h.Flags & 0x04) != 0;
            if (h.SignatureOffset != 0)
            {
                var sig = CXEXParser.ParseSignature(data, h.SignatureOffset);
                AnsiConsole.MarkupLine("");
                AnsiConsole.MarkupLine($"[bold]CXSG[/]");
                AnsiConsole.MarkupLine($"  sig_algo      {sig.SigAlgo} {(sig.SigAlgo == 1 ? "(RSA2048-SHA256)" : "")}");
                AnsiConsole.MarkupLine($"  hash_algo     {sig.HashAlgo} {(sig.HashAlgo == 1 ? "(SHA256)" : "")}");
                AnsiConsole.MarkupLine($"  sig_len       {sig.SigLen}");
                AnsiConsole.MarkupLine($"  fingerprint   [grey]{Convert.ToHexString(sig.Fingerprint).ToLowerInvariant()}[/]");
                if (!flagSigned)
                    AnsiConsole.MarkupLine("  [yellow]warning:[/] signature present but FLAG_SIGNED (bit 2) is not set.");
            }
            else if (flagSigned)
            {
                AnsiConsole.MarkupLine("[yellow]warning:[/] FLAG_SIGNED is set but signature_offset is 0.");
            }
        }
        catch (Exception ex)
        {
            AnsiConsole.MarkupLine($"[red]Error:[/] {ex.Message}");
            return 1;
        }

        return 0;
    }

    // phys_base lives at offset 48; the FileType parser stops at 44, so read it directly.
    private static uint ReadPhysBase(byte[] data)
        => data.Length >= 52 ? CXEX.Core.Utilities.MemoryPrimitives.ReadU32(data, 48) : 0;

    private static string DescribeFlags(uint f)
    {
        // bit values from kernel/lib/format/cxex.h (CXEX_FLAG_*)
        var parts = new System.Collections.Generic.List<string>();
        if ((f & (1u << 0)) != 0) parts.Add("EXECUTABLE");
        if ((f & (1u << 1)) != 0) parts.Add("RELOCATABLE");
        if ((f & (1u << 2)) != 0) parts.Add("SIGNED");
        if ((f & (1u << 3)) != 0) parts.Add("KERNEL_PRIV");
        if ((f & (1u << 4)) != 0) parts.Add("REQUIRE_ABI_MATCH");
        if ((f & (1u << 5)) != 0) parts.Add("REQUIRE_ARCH_MATCH");
        return parts.Count == 0 ? "" : string.Join(" | ", parts);
    }

    private static string DescribeSecFlags(uint f)
    {
        // CXEX_SEC_READ 1, _WRITE 2, _EXEC 4, _NOBITS 8 (per cxex.h)
        Span<char> rwx = stackalloc char[3];
        rwx[0] = (f & 1) != 0 ? 'r' : '-';
        rwx[1] = (f & 2) != 0 ? 'w' : '-';
        rwx[2] = (f & 4) != 0 ? 'x' : '-';
        string s = new string(rwx);
        if ((f & 8) != 0) s += " (nobits)";
        return s;
    }
}
