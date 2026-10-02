// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Core.Constants;
using CXEX.Core.Utilities;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// Builds a minimal, well-formed CXEX executable image, so each adversarial test
/// can state its case as one mutation of something known good.
///
/// <para>Hand-written rather than taken from the build output on purpose: a test
/// that reads <c>dist/CXK_x86_32/packages/kernel.xkex</c> only runs where the OS
/// has been built, and silently changes meaning whenever the kernel does. These
/// bytes are fixed and
/// run anywhere .NET does, which is what keeps the Adversarial category free of
/// the Toolchain requirement.</para>
/// </summary>
public sealed class CxexBuilder
{
    public const int HeaderSize = 56;
    public const int SectionEntrySize = 28;

    public ushort TypeCode { get; set; } = 0x5545;   // 'UE', user-mode
    public ushort FormatVersion { get; set; } = 1;
    public ushort ArchTarget { get; set; } = 1;
    public ushort AbiVersion { get; set; } = 1;
    public uint Flags { get; set; } = 0;
    public uint EntryPoint { get; set; } = 0x00400000;
    public uint LoadBase { get; set; } = 0x00400000;
    public uint ImageMin { get; set; } = 0x00400000;
    public uint ImageMax { get; set; } = 0x00400040;

    /// <summary>Overrides e_section_count. Null means "however many sections there are".</summary>
    public ushort? SectionCountOverride { get; set; }

    /// <summary>Overrides e_section_offset. Null means 56 (right after the header).</summary>
    public ushort? SectionOffsetOverride { get; set; }

    public uint RelocOffset { get; set; } = 0;
    public uint SignatureOffset { get; set; } = 0;
    public uint DependencyOffset { get; set; } = 0;
    public uint PhysBase { get; set; } = 0;

    public List<Section> Sections { get; } = new();

    /// <summary>One section in the CXEX section table.</summary>
    public sealed class Section
    {
        public string Name = string.Empty;
        public uint FileOffset;
        public uint VirtualAddress;
        public uint FileSize;
        public uint MemSize;
        public uint Flags;
        public byte[] Payload = Array.Empty<byte>();
    }

    /// <summary>
    /// A single executable section: one <c>.text</c> at virtual address 0x00400000,
    /// 64 bytes of payload, file_offset placed right after the section table,
    /// entry_point = 0x00400000, load_base = image_min = 0x00400000,
    /// image_max = 0x00400040, signature_offset = 0.
    /// </summary>
    public static CxexBuilder Valid(int codeBytes = 64)
    {
        int secTableSize = 1 * SectionEntrySize;
        uint fileOffset = (uint)(HeaderSize + secTableSize);

        var b = new CxexBuilder();
        b.Sections.Add(new Section
        {
            Name = ".text",
            FileOffset = fileOffset,
            VirtualAddress = 0x00400000,
            FileSize = (uint)codeBytes,
            MemSize = (uint)codeBytes,
            // Read + execute. NOT writable: a W+X baseline would be the very thing
            // the W^X rule exists to refuse, and every test built on it would then
            // pass or fail for a reason other than the one it names.
            Flags = CXFlags.SEC_READ | CXFlags.SEC_EXEC,
            Payload = Enumerable.Range(0, codeBytes).Select(i => (byte)(0x90 + (i & 7))).ToArray(),
        });
        b.EntryPoint = 0x00400000;
        b.ImageMax = 0x00400000 + (uint)codeBytes;
        return b;
    }

    public CxexBuilder With(Action<CxexBuilder> mutate)
    {
        mutate(this);
        return this;
    }

    /// <summary>Mutates section <paramref name="index"/>, for the single-field adversarial cases.</summary>
    public CxexBuilder WithSection(int index, Action<Section> mutate)
    {
        mutate(Sections[index]);
        return this;
    }

    public byte[] Build()
    {
        // The file is sized from the header, the section table and the payloads —
        // deliberately NOT from SectionOffsetOverride or an out-of-range FileOffset.
        // Sizing it to cover those would quietly repair the very malformation a test
        // is expressing: "section table past the end" became "a 2 GB file whose
        // table is exactly in bounds", and the parser was right to accept it.
        int secCount = Sections.Count;
        long end = HeaderSize + secCount * SectionEntrySize;

        foreach (Section sec in Sections)
        {
            if (sec.Payload.Length == 0) continue;
            long stop = (long)sec.FileOffset + sec.Payload.Length;
            // The upper bound keeps a deliberately absurd FileOffset from asking
            // for a gigabyte-long array; such a case wants a small file and an
            // out-of-range offset, which is exactly what skipping the growth gives.
            if (stop > end && stop < 1 << 20) end = stop;
        }

        byte[] file = new byte[end];
        Span<byte> span = file;

        // 56-byte CXEX header
        MemoryPrimitives.WriteU32(span, 0, 0x58455843);   // 'CXEX'
        MemoryPrimitives.WriteU16(span, 4, TypeCode);
        MemoryPrimitives.WriteU16(span, 6, FormatVersion);
        MemoryPrimitives.WriteU16(span, 8, ArchTarget);
        MemoryPrimitives.WriteU16(span, 10, AbiVersion);
        MemoryPrimitives.WriteU32(span, 12, Flags);
        MemoryPrimitives.WriteU32(span, 16, EntryPoint);
        MemoryPrimitives.WriteU32(span, 20, LoadBase);
        MemoryPrimitives.WriteU32(span, 24, ImageMin);
        MemoryPrimitives.WriteU32(span, 28, ImageMax);
        MemoryPrimitives.WriteU16(span, 32, SectionCountOverride ?? (ushort)secCount);
        MemoryPrimitives.WriteU16(span, 34, SectionOffsetOverride ?? 56);
        MemoryPrimitives.WriteU32(span, 36, RelocOffset);
        MemoryPrimitives.WriteU32(span, 40, SignatureOffset);
        MemoryPrimitives.WriteU32(span, 44, DependencyOffset);
        MemoryPrimitives.WriteU32(span, 48, PhysBase);
        // bytes 52..55 are zero (default byte array)

        // Section table
        int secTablePos = SectionOffsetOverride ?? HeaderSize;
        for (int i = 0; i < Sections.Count; i++)
        {
            // Guards against intentionally malformed offsets (e.g. near 0xFFFFFFFF
            // which cast to a negative int and slip past an upper-bound test).
            long at64 = (long)secTablePos + (long)i * SectionEntrySize;
            if (at64 < 0 || at64 + SectionEntrySize > file.Length) break;
            int at = (int)at64;

            Section sec = Sections[i];

            // 8-byte NUL-padded name
            byte[] nameBytes = System.Text.Encoding.ASCII.GetBytes(sec.Name);
            int nameLen = Math.Min(nameBytes.Length, 8);
            nameBytes.AsSpan(0, nameLen).CopyTo(span.Slice(at));

            MemoryPrimitives.WriteU32(span, at + 8, sec.FileOffset);
            MemoryPrimitives.WriteU32(span, at + 12, sec.VirtualAddress);
            MemoryPrimitives.WriteU32(span, at + 16, sec.FileSize);
            MemoryPrimitives.WriteU32(span, at + 20, sec.MemSize);
            MemoryPrimitives.WriteU32(span, at + 24, sec.Flags);

            // Copy raw payload into place
            if (sec.Payload.Length > 0)
            {
                long payloadPos64 = (long)sec.FileOffset;
                if (payloadPos64 >= 0 && payloadPos64 + sec.Payload.Length <= file.Length)
                    sec.Payload.CopyTo(span.Slice((int)payloadPos64, sec.Payload.Length));
            }
        }

        return file;
    }
}
