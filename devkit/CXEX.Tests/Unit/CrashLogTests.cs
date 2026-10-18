// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text;
using CXEX.Build.Engines;
using CXEX.Core.Constants;
using CXEX.Disk;
using CXEX.Tools;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// The DevKit's half of the crash log (roadmap step 4): the CRASH partition in
/// every image, the XCRL record as <c>cxk rig crashlog</c> reads it, and how
/// <c>cxk verify-crashlog</c> judges the boot that panics on request. The
/// kernel's half is kernel/test/host/test_crashrec.c; the two meet in
/// <c>cxk verify-crashlog</c>, where each reads what the other wrote.
/// </summary>
public class CrashLogTests
{
    /// <summary>A 4 MiB image with an XBPT table whose second entry is a CRASH partition.</summary>
    private static MemoryStream Image(bool withCrash = true)
    {
        var img = new byte[4 * 1024 * 1024];
        var t = img.AsSpan(512, 512);
        Encoding.ASCII.GetBytes("XBPT").CopyTo(t);
        BitConverter.TryWriteBytes(t[4..], (ushort)1);
        BitConverter.TryWriteBytes(t[6..], (ushort)(withCrash ? 2 : 1));
        BitConverter.TryWriteBytes(t[8..], (ushort)32);
        BitConverter.TryWriteBytes(t[12..], (ulong)(img.Length / 512));
        void Entry(int i, ulong start, ulong count, byte type, string name)
        {
            var e = img.AsSpan(512 + 32 + i * 32, 32);
            BitConverter.TryWriteBytes(e, start);
            BitConverter.TryWriteBytes(e[8..], count);
            e[16] = type;
            Encoding.ASCII.GetBytes(name).CopyTo(e[20..]);
        }
        Entry(0, 2048, 2048, CXFlags.PART_CXFS, "SYSTEM");
        if (withCrash) Entry(1, 4096, 2048, CXFlags.PART_CXCRASH, "CRASH");
        return new MemoryStream(img, writable: true);
    }

    private static void PutRecord(MemoryStream img, ushort kind, byte[] text)
    {
        img.Seek(4096L * 512, SeekOrigin.Begin);
        img.Write(CrashLog.Seal(kind, text));
        img.Write(text);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_crc_is_ieee_as_the_kernels()
    {
        // The check value, the same one test_crashrec.c holds the kernel to.
        Assert.Equal(0xCBF43926u, CrashLog.Crc32(0, Encoding.ASCII.GetBytes("123456789")));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Every_image_carries_a_crash_partition_before_the_system()
    {
        var map = DiskGeometryEngine.CalculateLayout(64, 4, 16 * 512, 0);
        var crash = Assert.Single(map.Partitions, p => p.TypeCode == CXFlags.PART_CXCRASH);
        var system = Assert.Single(map.Partitions, p => p.TypeCode == CXFlags.PART_CXFS);
        Assert.True(crash.SectorCount >= (ulong)CrashLog.Sectors);
        Assert.Equal(crash.StartLba + crash.SectorCount, system.StartLba);
        Assert.Equal(0ul, crash.StartLba % 2048);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_record_reads_back_and_a_cleared_area_reads_empty()
    {
        using var img = Image();
        Assert.Equal(CrashAreaState.Empty, CrashLog.Read(img).State);

        var text = Encoding.ASCII.GetBytes("*** KERNEL PANIC ***\nInvariant violated: test\n");
        PutRecord(img, CrashLog.KindCrash, text);
        var r = CrashLog.Read(img);
        Assert.Equal(CrashAreaState.Record, r.State);
        Assert.Equal(Encoding.ASCII.GetString(text), r.Text);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Arming_writes_a_request_the_reader_recognises()
    {
        using var img = Image();
        CrashLog.Arm(img);
        Assert.Equal(CrashAreaState.Request, CrashLog.Read(img).State);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_torn_or_foreign_record_is_refused()
    {
        using var img = Image();
        var text = Encoding.ASCII.GetBytes("Invariant violated: the record must be whole.\n");
        PutRecord(img, CrashLog.KindCrash, text);
        img.Seek(4097L * 512 + 5, SeekOrigin.Begin);
        img.WriteByte((byte)'#');                                        // text torn
        Assert.Equal(CrashAreaState.Invalid, CrashLog.Read(img).State);

        PutRecord(img, CrashLog.KindCrash, text);
        img.Seek(4096L * 512 + 4, SeekOrigin.Begin);
        img.WriteByte(2);                                                // a newer version
        var r = CrashLog.Read(img);
        Assert.Equal(CrashAreaState.Invalid, r.State);
        Assert.Contains("version", r.Detail);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_disk_without_the_partition_has_no_area()
    {
        using var img = Image(withCrash: false);
        Assert.Equal(CrashAreaState.NoArea, CrashLog.Read(img).State);
        Assert.Throws<InvalidOperationException>(() => CrashLog.Arm(img));
    }

    private const string Saved =
        "[KTEST   ] - [OK  ] - self-tests: all 61 passed\n*** KERNEL PANIC ***\n" +
        "Invariant violated: deliberate panic: a crash-log test asked for one\nCalled from EIP: 0xC0100721\n" +
        "Crash log saved; the next boot copies it to /System/crash.log\nSystem halted.";

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_requested_panic_passes_only_when_saved()
    {
        Assert.Equal(BootOutcome.Pass, BootVerdict.ClassifyRequestedPanic(Saved).Outcome);
        // the console wraps a line where it reaches its width, mid-word too
        Assert.Equal(BootOutcome.Pass, BootVerdict.ClassifyRequestedPanic(Saved.Replace("asked for", "asked fo\r\nr")).Outcome);

        Assert.Equal(BootOutcome.Fail, BootVerdict.ClassifyRequestedPanic(
            Saved.Replace("Crash log saved; the next boot copies it to /System/crash.log", "Crash log not saved, code 0xFFFFFFFE")).Outcome);
        Assert.Equal(BootOutcome.Fail, BootVerdict.ClassifyRequestedPanic(
            Saved.Replace("deliberate panic: a crash-log test asked for one", "page fault")).Outcome);
        Assert.Equal(BootOutcome.Fail, BootVerdict.ClassifyRequestedPanic(
            "[KTEST   ] - [OK  ] - self-tests: all 61 passed\n[EXEC    ] - [OK  ] - executive started, pid 1\n").Outcome);
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.ClassifyRequestedPanic("[KTEST   ] - [OK  ] - self-tests: all 61 passed\n").Outcome);
    }
}
