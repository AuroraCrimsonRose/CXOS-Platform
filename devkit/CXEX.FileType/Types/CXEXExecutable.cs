using System;
using System.Collections.Generic;
using CXEX.Core.Interfaces;
using CXEX.FileType.Structures;
using CXEX.FileType.Parsers;

namespace CXEX.FileType.Types;

public class CXEXExecutable : ICXFile
{
    public CXEXHeader Header { get; private set; } = null!;
    public List<CXEXSection> Sections { get; private set; } = new();
    public CXEXSignatureBlock? Signature { get; private set; }

    // Holds the raw file backing so we can extract sections later
    private byte[] _rawData = Array.Empty<byte>();

    public string GetDisplayName()
    {
        return Header?.TypeCode switch
        {
            0x4B45 => "CXK Protected Kernel Executable (.xkex)",
            0x4245 => "CXK Boot Executive (.xbex)",
            0x4F45 => "CXOS Executive (.xoex)",
            0x5345 => "CXOS System Program (.xsex)",
            0x5545 => "CXOS User Application (.xuex)",
            // Retired: .xcex named an executable for how it was built, and
            // every executable is compiled. Still recognised so an image from
            // before the split reports something useful rather than "unknown".
            0x4345 => "CXOS User Application (.xcex, retired - rebuild as .xuex)",
            _ => "Unknown CXEX Object"
        };
    }

    /// <summary>
    /// The image format version this understands. An image declaring anything else
    /// is refused rather than read with these field offsets, since a future version
    /// is free to move them.
    /// </summary>
    private const ushort SupportedFormatVersion = 1;

    /// <summary>
    /// A real CXEX has a handful of sections - the kernel, the largest, has five.
    /// The cap refuses an absurd section_count on sight instead of walking 65535
    /// entries across a file that cannot hold them.
    /// </summary>
    private const int MaxSections = 256;

    /// <summary>
    /// Parses and fully validates a CXEX image. A <see cref="CXEXExecutable"/> that
    /// loaded without throwing is the validated model: the section table lies
    /// inside the file, no section's bytes fall outside it, no arithmetic wraps, no
    /// two sections overlap in memory, and - when the image is signed - every byte
    /// any section contributes lies inside the signed range.
    ///
    /// <para>This used to take <c>section_count</c> on trust and walk the table
    /// until something further down threw. The reviews ask the DevKit and CXK to
    /// validate independently (security §11), so these checks deliberately repeat
    /// what the kernel's loader does rather than assuming either side ran first.</para>
    /// </summary>
    /// <exception cref="InvalidDataException">The image is malformed. The message names the field.</exception>
    public void Load(byte[] data)
    {
        ArgumentNullException.ThrowIfNull(data);

        _rawData = data;
        ReadOnlySpan<byte> span = data;
        Sections.Clear();
        Signature = null;

        Header = CXEXParser.ParseHeader(span);

        if (Header.FormatVersion != SupportedFormatVersion)
            throw new InvalidDataException(
                $"CXEX format version {Header.FormatVersion} is not supported (this reads version {SupportedFormatVersion}).");

        if (Header.SectionCount > MaxSections)
            throw new InvalidDataException(
                $"CXEX declares {Header.SectionCount} sections, beyond the {MaxSections} this accepts.");

        if (Header.SectionOffset < CXEXParser.HEADER_SIZE)
            throw new InvalidDataException(
                $"section_offset {Header.SectionOffset} overlaps the {CXEXParser.HEADER_SIZE}-byte header.");

        // In 64 bits: both terms are bounded above, but the point of the check is
        // that a narrow sum can wrap and land back inside the file.
        long tableEnd = (long)Header.SectionOffset + (long)Header.SectionCount * CXEXParser.SECTION_SIZE;
        if (tableEnd > span.Length)
            throw new InvalidDataException(
                $"the section table runs past the end of the file: section_offset {Header.SectionOffset} + " +
                $"{Header.SectionCount} x {CXEXParser.SECTION_SIZE} = {tableEnd}, file is {span.Length} bytes.");

        // The signature block, when present, bounds everything else: the kernel
        // hashes [0, signature_offset), so any byte at or beyond it is unsigned.
        bool flagSigned = (Header.Flags & 0x04) != 0;
        if (flagSigned != (Header.SignatureOffset != 0))
            throw new InvalidDataException(
                $"FLAG_SIGNED is {(flagSigned ? "set" : "clear")} but signature_offset is {Header.SignatureOffset}: " +
                "the two must agree, or it is ambiguous whether the image claims to be signed.");

        if (Header.SignatureOffset != 0 && Header.SignatureOffset > span.Length)
            throw new InvalidDataException(
                $"signature_offset {Header.SignatureOffset} is past the end of a {span.Length}-byte file.");

        for (int i = 0; i < Header.SectionCount; i++)
        {
            int at = Header.SectionOffset + i * CXEXParser.SECTION_SIZE;   // bounded by tableEnd
            CXEXSection sec = CXEXParser.ParseSection(span, at);

            if (sec.FileSize > sec.MemSize)
                throw new InvalidDataException(
                    $"section {i} ('{sec.Name}'): file_size {sec.FileSize} exceeds mem_size {sec.MemSize}.");

            if ((long)sec.VirtAddr + sec.MemSize > uint.MaxValue)
                throw new InvalidDataException(
                    $"section {i} ('{sec.Name}'): virt_addr 0x{sec.VirtAddr:X8} + mem_size {sec.MemSize} " +
                    "leaves the 32-bit address space.");

            if (sec.FileSize > 0)
            {
                long fileEnd = (long)sec.FileOffset + sec.FileSize;
                if (fileEnd > span.Length)
                    throw new InvalidDataException(
                        $"section {i} ('{sec.Name}'): file_offset {sec.FileOffset} + file_size {sec.FileSize} = " +
                        $"{fileEnd}, past the end of a {span.Length}-byte file.");

                if (sec.FileOffset < tableEnd)
                    throw new InvalidDataException(
                        $"section {i} ('{sec.Name}'): file_offset {sec.FileOffset} falls inside the header or " +
                        $"section table, which end at {tableEnd}.");

                // The signed range is [0, signature_offset). A section whose bytes
                // start at or run past it is data the signature does not cover, and
                // the loader would still map it - which is the whole point of the
                // "signature must cover every byte the loader reads" finding.
                if (Header.SignatureOffset != 0 && fileEnd > Header.SignatureOffset)
                    throw new InvalidDataException(
                        $"section {i} ('{sec.Name}'): bytes [{sec.FileOffset},{fileEnd}) extend beyond " +
                        $"signature_offset {Header.SignatureOffset}, so they are not covered by the signature.");
            }

            Sections.Add(sec);
        }

        for (int a = 0; a < Sections.Count; a++)
        {
            for (int b = a + 1; b < Sections.Count; b++)
            {
                CXEXSection x = Sections[a], y = Sections[b];
                if (x.MemSize == 0 || y.MemSize == 0) continue;

                long xEnd = (long)x.VirtAddr + x.MemSize;
                long yEnd = (long)y.VirtAddr + y.MemSize;
                if (x.VirtAddr < yEnd && y.VirtAddr < xEnd)
                    throw new InvalidDataException(
                        $"sections {a} ('{x.Name}') and {b} ('{y.Name}') overlap in memory: " +
                        $"[0x{x.VirtAddr:X8},0x{xEnd:X8}) and [0x{y.VirtAddr:X8},0x{yEnd:X8}).");
            }
        }

        if (Header.SignatureOffset != 0)
            Signature = CXEXParser.ParseSignature(span, Header.SignatureOffset);
    }

    public byte[] GetSectionData(string sectionName)
    {
        var section = Sections.Find(s => s.Name == sectionName);
        if (section == null || section.FileSize == 0)
            return Array.Empty<byte>();

        // Load has already proved this range is inside the file. Checked again
        // because Sections is public and mutable, so this is reachable with a
        // section Load never saw - and silently copying from the wrong offset is
        // worse than refusing.
        if ((long)section.FileOffset + section.FileSize > _rawData.Length)
            throw new InvalidDataException(
                $"section '{sectionName}': bytes [{section.FileOffset},{(long)section.FileOffset + section.FileSize}) " +
                $"are outside the {_rawData.Length}-byte image.");

        byte[] dest = new byte[section.FileSize];
        Array.Copy(_rawData, section.FileOffset, dest, 0, section.FileSize);
        return dest;
    }
}