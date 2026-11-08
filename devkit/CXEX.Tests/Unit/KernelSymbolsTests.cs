// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Buffers.Binary;
using System.Text;
using CXEX.Build.Parsers;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The reader behind <c>cxk ksyms</c>: what goes in the table a panic names its
/// callers with, and that a damaged file is refused rather than crashing it.
/// </summary>
public class KernelSymbolsTests
{
    private const ulong TextAddr = 0xFFFFFFFF80100000;
    private const int TextSize = 0x1000;
    private const byte Func = 2, NoType = 0, Object = 1;
    private const ushort TextIndex = 1, OtherIndex = 2;

    private sealed record Sym(string Name, byte Type, ushort Section, ulong Value);

    /// <summary>
    /// An ELF64 with sections null, .text, .data, .symtab, .strtab, .shstrtab,
    /// the section table last. <paramref name="withSymtab"/> false is a stripped link.
    /// </summary>
    private static byte[] Elf(IEnumerable<Sym> syms, bool withSymtab = true)
    {
        var strtab = new MemoryStream();
        strtab.WriteByte(0);
        var symtab = new MemoryStream();
        symtab.Write(new byte[24]);
        foreach (var s in syms)
        {
            var e = new byte[24];
            BinaryPrimitives.WriteUInt32LittleEndian(e, (uint)strtab.Position);
            e[4] = s.Type;
            BinaryPrimitives.WriteUInt16LittleEndian(e.AsSpan(6), s.Section);
            BinaryPrimitives.WriteUInt64LittleEndian(e.AsSpan(8), s.Value);
            symtab.Write(e);
            strtab.Write(Encoding.ASCII.GetBytes(s.Name));
            strtab.WriteByte(0);
        }

        string[] sectionNames = { "", ".text", ".data", ".symtab", ".strtab", ".shstrtab" };
        var shstr = new MemoryStream();
        var nameAt = new uint[sectionNames.Length];
        for (int i = 0; i < sectionNames.Length; i++)
        {
            nameAt[i] = (uint)shstr.Position;
            shstr.Write(Encoding.ASCII.GetBytes(sectionNames[i]));
            shstr.WriteByte(0);
        }

        var file = new MemoryStream();
        file.Write(new byte[64]);
        long symOff = file.Position; file.Write(symtab.ToArray());
        long strOff = file.Position; file.Write(strtab.ToArray());
        long shstrOff = file.Position; file.Write(shstr.ToArray());
        while (file.Position % 8 != 0) file.WriteByte(0);
        long shoff = file.Position;

        void Section(int name, uint type, ulong addr, ulong off, ulong size, uint link)
        {
            var sh = new byte[64];
            BinaryPrimitives.WriteUInt32LittleEndian(sh, nameAt[name]);
            BinaryPrimitives.WriteUInt32LittleEndian(sh.AsSpan(4), type);
            BinaryPrimitives.WriteUInt64LittleEndian(sh.AsSpan(0x10), addr);
            BinaryPrimitives.WriteUInt64LittleEndian(sh.AsSpan(0x18), off);
            BinaryPrimitives.WriteUInt64LittleEndian(sh.AsSpan(0x20), size);
            BinaryPrimitives.WriteUInt32LittleEndian(sh.AsSpan(0x28), link);
            file.Write(sh);
        }
        Section(0, 0, 0, 0, 0, 0);
        Section(1, 8, TextAddr, 0, TextSize, 0);
        Section(2, 8, TextAddr + TextSize, 0, 0x100, 0);
        Section(3, withSymtab ? 2u : 1u, 0, (ulong)symOff, (ulong)symtab.Length, 4);
        Section(4, 3, 0, (ulong)strOff, (ulong)strtab.Length, 0);
        Section(5, 3, 0, (ulong)shstrOff, (ulong)shstr.Length, 0);

        byte[] b = file.ToArray();
        new byte[] { 0x7F, (byte)'E', (byte)'L', (byte)'F', 2, 1 }.CopyTo(b, 0);
        BinaryPrimitives.WriteUInt64LittleEndian(b.AsSpan(0x28), (ulong)shoff);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(0x3A), 64);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(0x3C), 6);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(0x3E), 5);
        return b;
    }

    private static readonly Sym[] Good =
    {
        new("second", Func, TextIndex, TextAddr + 0x200),
        new("first", Func, TextIndex, TextAddr + 0x100),
    };

    [Fact]
    public void Functions_come_out_as_offsets_from_text_in_address_order()
    {
        var read = KernelSymbols.Read(Elf(Good));
        Assert.Equal(new[] { new KernelSymbol(0x100, "first"), new KernelSymbol(0x200, "second") }, read);
    }

    [Fact]
    public void An_assembly_label_counts_but_a_function_at_its_address_wins()
    {
        var read = KernelSymbols.Read(Elf(new[]
        {
            new Sym("aaa_label", NoType, TextIndex, TextAddr + 0x10),
            new Sym("the_function", Func, TextIndex, TextAddr + 0x10),
            new Sym("lone_label", NoType, TextIndex, TextAddr + 0x20),
        }));
        Assert.Equal(new[] { new KernelSymbol(0x10, "the_function"), new KernelSymbol(0x20, "lone_label") }, read);
    }

    [Fact]
    public void What_is_not_a_function_in_text_is_left_out()
    {
        var read = KernelSymbols.Read(Elf(new[]
        {
            new Sym("kept", Func, TextIndex, TextAddr),
            new Sym("a_variable", Object, TextIndex, TextAddr + 8),
            new Sym("in_data", Func, OtherIndex, TextAddr + 16),
            new Sym("before_text", Func, TextIndex, TextAddr - 1),
            new Sym("past_text", Func, TextIndex, TextAddr + TextSize),
            new Sym(".Llocal", NoType, TextIndex, TextAddr + 24),
            new Sym("__text_start", NoType, TextIndex, TextAddr + 32),
            new Sym("bad\"name", Func, TextIndex, TextAddr + 40),
        }));
        Assert.Equal(new[] { new KernelSymbol(0, "kept") }, read);
    }

    [Fact]
    public void A_stripped_link_is_an_error()
    {
        var e = Assert.Throws<InvalidDataException>(() => KernelSymbols.Read(Elf(Good, withSymtab: false)));
        Assert.Contains("stripped", e.Message);
    }

    [Fact]
    public void Text_with_no_functions_is_an_error()
    {
        Assert.Throws<InvalidDataException>(() => KernelSymbols.Read(Elf(Array.Empty<Sym>())));
    }

    /// <summary>Cut off anywhere, the file is refused or read; nothing else.</summary>
    [Fact]
    public void A_truncated_file_never_crashes_the_reader()
    {
        byte[] whole = Elf(Good);
        for (int len = 0; len < whole.Length; len++)
        {
            byte[] cut = whole[..len];
            try { KernelSymbols.Read(cut); }
            catch (InvalidDataException) { }
        }
    }

    /// <summary>
    /// Every header field and table entry set to the values that break range
    /// arithmetic: refused or read, never an out-of-range exception.
    /// </summary>
    [Fact]
    public void A_corrupted_file_never_crashes_the_reader()
    {
        byte[] whole = Elf(Good);
        foreach (byte value in new byte[] { 0x00, 0x7F, 0x80, 0xFF })
            for (int at = 0; at < whole.Length; at++)
            {
                byte[] bad = (byte[])whole.Clone();
                bad[at] = value;
                try { KernelSymbols.Read(bad); }
                catch (InvalidDataException) { }
            }
    }

    /// <summary>A wholly huge offset: ulong arithmetic must not wrap into range.</summary>
    [Theory]
    [InlineData(0x28)]              // the section table's offset
    public void An_offset_near_the_top_of_the_address_space_is_refused(int field)
    {
        byte[] bad = Elf(Good);
        BinaryPrimitives.WriteUInt64LittleEndian(bad.AsSpan(field), ulong.MaxValue - 8);
        Assert.Throws<InvalidDataException>(() => KernelSymbols.Read(bad));
    }

    [Fact]
    public void The_generated_c_has_a_terminated_name_for_every_offset()
    {
        string c = KernelSymbols.ToC(new[] { new KernelSymbol(0x10, "ab"), new KernelSymbol(0x20, "cde") });
        Assert.Contains("const uint32_t ksym_count = 2;", c);
        Assert.Contains("0x10,\n0x20,\n", c);
        Assert.Contains("0,\n3,\n", c);     // "ab\0" is three bytes
        Assert.Contains("\"ab\\0\"\n\"cde\\0\"\n", c);
    }
}
