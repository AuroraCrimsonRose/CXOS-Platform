// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Disk;
using CXEX.Disk.Models;
using CXEX.Disk.Parsers;
using Xunit;

namespace CXEX.Tests.Adversarial;

/// <summary>
/// A GPT is untrusted input - the whole reason to inspect a disk image is that
/// you do not know what is in it - and <see cref="GptParser"/> trusted it
/// (platform security review 2026-10-09 §2).
///
/// <para>Every case is a single mutation of <see cref="GptBuilder.Valid"/>,
/// and the valid image is a case of its own, so the suite cannot pass by
/// rejecting everything.</para>
/// </summary>
public class GptParserTests
{
    private static DiskLayout Parse(GptBuilder b)
    {
        byte[] img = b.Build();
        using var s = new MemoryStream(img, writable: false);
        return GptParser.Parse(s, img.Length, GptBuilder.SectorSize);
    }

    private static void Refuses(GptBuilder b)
        => Assert.ThrowsAny<Exception>(() => Parse(b));

    // ---- the control ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_well_formed_gpt_parses_with_the_right_partitions()
    {
        var layout = Parse(GptBuilder.Valid());

        Assert.Equal(PartitionTableType.GPT, layout.TableType);
        Assert.Equal(2, layout.Partitions.Count);
        Assert.Equal(34, layout.Partitions[0].StartLba);
        Assert.Equal(67, layout.Partitions[0].SectorCount);       // 100 - 34 + 1
        Assert.Equal(34 * 512, layout.Partitions[0].StartOffset);
        Assert.Equal("system", layout.Partitions[0].Name);
        Assert.Equal("Linux filesystem", layout.Partitions[0].TypeName);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_dispatcher_reaches_the_gpt_parser_for_a_real_image()
    {
        // Analyze is the only caller, so a parser that is correct but
        // unreachable would still be a bug.
        byte[] img = GptBuilder.Valid().Build();
        using var s = new MemoryStream(img, writable: false);
        Assert.Equal(PartitionTableType.GPT, DiskAnalyzer.Analyze(s, img.Length).TableType);
    }

    // ---- the signature, checked here as well as in the dispatcher ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_bad_signature_is_refused_by_the_parser_itself()
    {
        // DiskAnalyzer checks "EFI PART" before dispatching, so this path is
        // unreachable through Analyze. Checked anyway: Parse is public, and a
        // guarantee that lives in a caller is no guarantee (security §11).
        var b = GptBuilder.Valid();
        b.CorruptSignature = true;
        Refuses(b);
    }

    // ---- the CRCs, neither of which was validated ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_corrupt_header_crc_is_refused()
    {
        var b = GptBuilder.Valid();
        b.CorruptHeaderCrc = true;
        Refuses(b);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_corrupt_entry_array_crc_is_refused()
    {
        var b = GptBuilder.Valid();
        b.CorruptEntriesCrc = true;
        Refuses(b);
    }

    // ---- the allocation arithmetic: the sharpest finding ----

    [Theory]
    [InlineData(0x01000000u)]   // 256 * this wraps uint to exactly 0
    [InlineData(0x00800000u)]   // ...and this to 0x80000000
    [InlineData(uint.MaxValue)]
    [InlineData(4097u)]         // just past the cap
    [InlineData(127u)]          // below a real entry
    [InlineData(0u)]
    [InlineData(132u)]          // plausible size, but not a multiple of 8
    [Trait(Categories.Key, Categories.Unit)]
    public void An_entry_size_outside_the_permitted_range_is_refused(uint entrySize)
    {
        // entrySize had no upper bound, and numEntries * entrySize wrapped as
        // uint BEFORE the cast to int: 256 * 0x01000000 == 0, giving a
        // zero-length buffer that the entry loop then indexed.
        var b = GptBuilder.Valid();
        b.EntrySizeField = entrySize;
        Refuses(b);
    }

    [Theory]
    [InlineData(0u)]
    [InlineData(257u)]
    [InlineData(uint.MaxValue)]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_absurd_entry_count_is_refused(uint numEntries)
    {
        var b = GptBuilder.Valid();
        b.NumEntries = numEntries;
        Refuses(b);
    }

    [Theory]
    [InlineData(ulong.MaxValue)]            // * 512 overflows
    [InlineData(0x0040000000000000ul)]      // * 512 overflows to a small value
    [InlineData(1_000_000ul)]               // in range arithmetically, past the image
    [InlineData(0ul)]                       // the protective MBR
    [Trait(Categories.Key, Categories.Unit)]
    public void An_entry_array_lba_that_overflows_or_leaves_the_image_is_refused(ulong lba)
    {
        var b = GptBuilder.Valid();
        b.EntryArrayLbaField = lba;
        Refuses(b);
    }

    // ---- truncation must fail, not parse as zeros ----

    [Theory]
    [InlineData(600)]     // header present, entry array cut off
    [InlineData(1100)]    // entry array partially present
    [InlineData(100)]     // header itself cut off
    [Trait(Categories.Key, Categories.Unit)]
    public void A_truncated_image_is_refused_rather_than_read_as_zeros(int bytes)
    {
        // ReadAt used to break out of its read loop on a short read and return
        // the zero-filled buffer, so a truncated image produced partitions made
        // of zeros - a silent wrong answer, which is worse than an exception.
        var b = GptBuilder.Valid();
        b.TruncateTo = bytes;
        Refuses(b);
    }

    /// <summary>A stream that reports more length than it will actually hand
    /// over. Not contrived: a network stream, a sparse file or a device that
    /// shrinks all behave this way.</summary>
    private sealed class LyingStream : Stream
    {
        private readonly MemoryStream _inner;
        private readonly long _claimed;
        public LyingStream(byte[] data, long claimed)
        { _inner = new MemoryStream(data, writable: false); _claimed = claimed; }

        public override long Length => _claimed;          // the lie
        public override bool CanRead => true;
        public override bool CanSeek => true;
        public override bool CanWrite => false;
        public override long Position { get => _inner.Position; set => _inner.Position = value; }
        public override long Seek(long o, SeekOrigin r) => _inner.Seek(o, r);
        public override int Read(byte[] b, int o, int c) => _inner.Read(b, o, c);
        public override void Flush() { }
        public override void SetLength(long v) => throw new NotSupportedException();
        public override void Write(byte[] b, int o, int c) => throw new NotSupportedException();
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_stream_that_under_delivers_is_refused_rather_than_zero_filled()
    {
        // Asserts the PROPERTY - under-delivery is refused - and deliberately
        // not which check refuses it.
        //
        // Worth recording what sabotage showed here, because it changes how
        // much this test is worth. Making ReadExact tolerate a short read (the
        // pre-fix behaviour) turns NOTHING red, including this. Three checks
        // sit in front of it: the bounds test against the stream length
        // catches an honestly-truncated image, and the entry-array CRC catches
        // any under-delivery at all, since zero-filling the tail changes the
        // checksum. ReadExact's throw is therefore unfalsifiable by
        // construction - no input can reach it that the CRC would not also
        // reject.
        //
        // It stays because it reports the real problem ("image ends after 900
        // of 1536 bytes") instead of "CRC mismatch", and because the CRC is
        // what makes it redundant - a future caller that skipped CRCs would
        // need it. But it is defence in depth, not a tested line, and saying
        // so is better than implying a sabotage proved it.
        var b = GptBuilder.Valid();
        b.TruncateTo = 900;                          // header fine, array cut short
        byte[] img = b.Build();

        using var s = new LyingStream(img, 256 * 512);   // claims the full image
        Assert.ThrowsAny<Exception>(
            () => GptParser.Parse(s, 256 * 512, GptBuilder.SectorSize));
    }

    // ---- partition ranges ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_partition_running_backwards_or_past_the_end_is_dropped()
    {
        // Dropped rather than thrown: one nonsensical entry in an otherwise
        // valid table should not make the whole disk unreadable, and the
        // alternative was StartOffset and SizeBytes coming out negative.
        var b = GptBuilder.Valid();
        b.Partitions[0].First = 200;
        b.Partitions[0].Last = 100;                 // backwards
        b.Partitions[1].First = 900_000;            // past the end
        b.Partitions[1].Last = 900_100;

        Assert.Empty(Parse(b).Partitions);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_valid_partition_beside_an_invalid_one_still_parses()
    {
        var b = GptBuilder.Valid();
        b.Partitions[1].First = 900_000;            // past the end
        b.Partitions[1].Last = 900_100;

        var parts = Parse(b).Partitions;
        Assert.Single(parts);
        Assert.Equal("system", parts[0].Name);
    }

    // ---- sector size ----

    [Theory]
    [InlineData(0)]
    [InlineData(-512)]
    [InlineData(500)]     // not a power of two
    [Trait(Categories.Key, Categories.Unit)]
    public void A_nonsensical_sector_size_is_refused(int sectorSize)
    {
        byte[] img = GptBuilder.Valid().Build();
        using var s = new MemoryStream(img, writable: false);
        Assert.ThrowsAny<Exception>(() => GptParser.Parse(s, img.Length, sectorSize));
    }

    // ---- the CRC implementation itself ----

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_crc32_matches_the_known_check_value()
    {
        // "123456789" -> 0xCBF43926 is the standard CRC-32/ISO-HDLC check
        // value. Without this the two CRC tests above would pass against a
        // wrong implementation, since the builder uses the same function.
        byte[] data = System.Text.Encoding.ASCII.GetBytes("123456789");
        Assert.Equal(0xCBF43926u, GptParser.Crc32(data, 0, data.Length));
    }
}
