// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text;
using System.Text.RegularExpressions;
using CXEX.Core.Constants;
using CXEX.Disk;
using CXEX.Tests.Differential;
using CXEX.Tools;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// <c>cxk hw show</c>, <c>hw diff</c> and <c>rig hwreport</c> (roadmap 1.8b,
/// docs/hardware/CX_HW_REPORT.md §5): a report's lines named by what they
/// describe, two reports compared fact by fact with each difference judged
/// expected or a finding, what one report says is wrong with its own machine,
/// and the report read off a disk's crash area (XCRL kind 6).
/// </summary>
public class HwDiffTests
{
    /// <summary>A QEMU report's lines, cut down (from a 1.8a boot's serial log).</summary>
    private static readonly string[] Qemu =
    [
        "@hw\tboot\tbios\t00000000\t00000000000f5a10",
        "@hw\tcpu\tAuthenticAMD\t0f\t6b\t1\tQEMU Virtual CPU version 2.5+",
        "@hw\tcpuid\t00000001\t00\t00060fb1\t00000800\t80002001\t078bfbfd",
        "@hw\thypervisor\tTCGTCGTCGTCG\t40000001",
        "@hw\tcpuid\t40000000\t00\t40000001\t54474354\t43544743\t47435447",
        "@hw\tmem\t0000000000000000\t000000000009fc00\tusable\t00000001",
        "@hw\tmem\t0000000000100000\t00000000bfee0000\tusable\t00000001",
        "@hw\tmemsum\t2\t0\t00000000bff7fc00",
        "@hw\tacpi\trsdp\t00000000000f52c0\t00\tBOCHS\tbffe2328\t-\tok\t-",
        "@hw\tacpi\ttable\trsdp\tRSDT\t00000000bffe2328\t00000034\t01\tBOCHS\tBXPC\t00000001\tok",
        "@hw\tacpi\ttable\trsdt\tSSDT\t00000000bffe2400\t00000040\t01\tBOCHS\tBXPC\t00000001\tok",
        "@hw\tacpi\ttable\trsdt\tSSDT\t00000000bffe2500\t00000080\t01\tBOCHS\tBXPC\t00000001\tok",
        "@hw\tmadt\thead\tfee00000\t00000001",
        "@hw\tmadt\tcpu\t00\t00\t00000001",
        "@hw\tmadt\tioapic\t00\tfec00000\t00000000",
        "@hw\tmadt\tnmi\tff\t0000\t01",
        "@hw\tpci\t00:02.0\t1234\t1111\t03\t00\t00\t00\t1af4\t1100\t00\t00\tfd000008\t00000000\tfebf0000\t00000000\t00000000\t00000000",
        "@hw\tpci\t00:03.0\t8086\t100e\t02\t00\t00\t00\t1af4\t1100\t0b\t01\tfebc0000\t0000c001\t00000000\t00000000\t00000000\t00000000",
        "@hw\tpcisum\t2\t256",
        "@hw\tclock\tsource\ttsc",
        "@hw\tclock\ttsc-hz\t999847680",
        "@hw\tclock\ttsc-by\tpit",
        "@hw\tclock\ttsc-pit-hz\t999847680",
        "@hw\tclock\ttick-hz\t1000",
        "@hw\tclock\ttick-irq-hz\t1000",
        "@hw\tclock\tpmtimer\t0608\t24",
        "@hw\tclock\ttsc-pm-hz\t999994936",
    ];

    private static HwReport Report(IEnumerable<string> body) => HwReport.Parse(HwReport.Seal(body));

    /// <summary>The QEMU report with each line that starts with a key in <paramref name="edits"/> replaced (null drops it).</summary>
    private static HwReport With(params (string Starts, string? Line)[] edits)
    {
        var lines = new List<string>();
        foreach (string l in Qemu)
        {
            var e = edits.FirstOrDefault(x => l.StartsWith(x.Starts, StringComparison.Ordinal));
            if (e.Starts is null) lines.Add(l);
            else if (e.Line is not null) lines.Add(e.Line);
        }
        return Report(lines);
    }

    private static HwDifference One(IReadOnlyList<HwDifference> d)
    {
        Assert.True(d.Count == 1, "expected one difference, got: " + string.Join("; ", d));
        return d[0];
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Each_line_is_named_by_what_it_describes_and_a_repeated_key_is_numbered()
    {
        var facts = HwDiff.Facts(Report(Qemu));
        Assert.DoesNotContain(facts, f => f.Section is "begin" or "end");
        var pci = facts.Single(f => f.Key == "pci 00:03.0");
        Assert.Equal(("device", "100e"), pci.Fields[1]);
        Assert.Equal(("bar1", "0000c001"), pci.Fields[11]);
        Assert.Equal(["acpi rsdt SSDT", "acpi rsdt SSDT #2"], facts.Where(f => f.Key.Contains("SSDT")).Select(f => f.Key));
        Assert.Equal(("ebx", "00000800"), facts.Single(f => f.Key == "cpuid 00000001.00").Fields[1]);
        var nmi = facts.Single(f => f.Key == "madt nmi ff lint01");
        Assert.Equal([("flags", "0000"), ("lint", "01")], nmi.Fields);
        Assert.Equal([("port", "0608"), ("bits", "24")], facts.Single(f => f.Key == "clock pmtimer").Fields);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_report_compared_with_itself_differs_nowhere_and_says_nothing_is_wrong()
    {
        Assert.Empty(HwDiff.Compare(Report(Qemu), Report(Qemu)));
        Assert.Empty(HwDiff.Notes(Report(Qemu)));
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData("@hw\tboot", "@hw\tboot\tbios\t00000000\t00000000000f0000", "smbios")]
    [InlineData("@hw\tacpi\trsdp", "@hw\tacpi\trsdp\t00000000000f5000\t00\tBOCHS\tbffe2328\t-\tok\t-", "phys")]
    [InlineData("@hw\tacpi\ttable\trsdp", "@hw\tacpi\ttable\trsdp\tRSDT\t00000000bffd0000\t00000034\t01\tBOCHS\tBXPC\t00000001\tok", "phys")]
    [InlineData("@hw\tmadt\thead", "@hw\tmadt\thead\tfed00000\t00000001", "lapic-addr")]
    [InlineData("@hw\tmadt\tioapic", "@hw\tmadt\tioapic\t00\tfec01000\t00000000", "addr")]
    [InlineData("@hw\tpci\t00:03.0", "@hw\tpci\t00:03.0\t8086\t100e\t02\t00\t00\t00\t1af4\t1100\t0a\t01\tfeb00000\t0000c001\t00000000\t00000000\t00000000\t00000000", "irq-line")]
    [InlineData("@hw\tcpuid\t40000000", "@hw\tcpuid\t40000000\t00\t40000001\t4b4d564b\t564b4d56\t0000004d", "ebx")]
    [InlineData("@hw\tclock\ttsc-hz", "@hw\tclock\ttsc-hz\t1005000000", "value")]
    [InlineData("@hw\tclock\ttick-irq-hz", "@hw\tclock\ttick-irq-hz\t950", "value")]
    public void What_the_firmware_chooses_and_the_hypervisor_says_about_itself_is_expected(string starts, string line, string field)
    {
        var d = HwDiff.Compare(Report(Qemu), With((starts, line)));
        Assert.NotEmpty(d);
        Assert.All(d, x => Assert.True(x.Expected, $"{x.Key} {x.Field}: {x.A} -> {x.B} judged a finding ({x.Why})"));
        Assert.Contains(d, x => x.Field == field);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_BAR_that_moves_is_expected_and_one_whose_type_bits_change_is_a_finding()
    {
        string Bar0(string v) => $"@hw\tpci\t00:03.0\t8086\t100e\t02\t00\t00\t00\t1af4\t1100\t0b\t01\t{v}\t0000c001\t00000000\t00000000\t00000000\t00000000";
        Assert.True(One(HwDiff.Compare(Report(Qemu), With(("@hw\tpci\t00:03.0", Bar0("fe000000"))))).Expected);

        var prefetch = One(HwDiff.Compare(Report(Qemu), With(("@hw\tpci\t00:03.0", Bar0("fe000008")))));
        Assert.False(prefetch.Expected);
        Assert.Contains("type bits", prefetch.Why);

        var gone = One(HwDiff.Compare(Report(Qemu), With(("@hw\tpci\t00:03.0", Bar0("00000000")))));
        Assert.False(gone.Expected);

        // A 64-bit memory BAR (type 10b): the next dword is its high half, all address.
        string B64(string hi) => $"@hw\tpci\t00:03.0\t8086\t100e\t02\t00\t00\t00\t1af4\t1100\t0b\t01\tfebc0004\t{hi}\t00000000\t00000000\t00000000\t00000000";
        var a = With(("@hw\tpci\t00:03.0", B64("00000000")));
        var hi = One(HwDiff.Compare(a, With(("@hw\tpci\t00:03.0", B64("00000001")))));
        Assert.Equal("bar1", hi.Field);
        Assert.True(hi.Expected, hi.Why);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void What_the_machine_is_differs_as_a_finding()
    {
        var device = One(HwDiff.Compare(Report(Qemu), With(("@hw\tpci\t00:03.0", "@hw\tpci\t00:03.0\t8086\t10d3\t02\t00\t00\t00\t1af4\t1100\t0b\t01\tfebc0000\t0000c001\t00000000\t00000000\t00000000\t00000000"))));
        Assert.False(device.Expected);
        Assert.Equal(("device", "100e", "10d3"), (device.Field, device.A, device.B));

        var bit = One(HwDiff.Compare(Report(Qemu), With(("@hw\tcpuid\t00000001", "@hw\tcpuid\t00000001\t00\t00060fb1\t00000800\t80002001\t068bfbfd"))));
        Assert.False(bit.Expected);
        Assert.Equal("edx", bit.Field);
        Assert.Contains("bits 01000000", bit.Why);

        var bad = One(HwDiff.Compare(Report(Qemu), With(("@hw\tacpi\ttable\trsdp", "@hw\tacpi\ttable\trsdp\tRSDT\t00000000bffe2328\t00000034\t01\tBOCHS\tBXPC\t00000001\tbad"))));
        Assert.False(bad.Expected);
        Assert.Equal("state", bad.Field);

        var rate = One(HwDiff.Compare(Report(Qemu), With(("@hw\tclock\ttsc-pm-hz", "@hw\tclock\ttsc-pm-hz\t2000000000"))));
        Assert.False(rate.Expected);
        Assert.Contains("over 1%", rate.Why);
        Assert.True(One(HwDiff.Compare(Report(Qemu), With(("@hw\tclock\ttsc-pm-hz", "@hw\tclock\ttsc-pm-hz\t2000000000")), 0.6)).Expected);

        var merged = One(HwDiff.Compare(Report(Qemu), With(("@hw\tclock\ttick-irq-hz", "@hw\tclock\ttick-irq-hz\t64"))));
        Assert.False(merged.Expected);
        Assert.Contains("over 10%", merged.Why);
        var how = One(HwDiff.Compare(Report(Qemu), With(("@hw\tclock\ttsc-by", "@hw\tclock\ttsc-by\ttick"))));
        Assert.Equal(("clock tsc-by", "pit", "tick"), (how.Key, how.A, how.B));
        Assert.False(how.Expected);

        var cpus = HwDiff.Compare(Report(Qemu), Report(Qemu.Append("@hw\tmadt\tcpu\t01\t01\t00000001")));
        var extra = One(cpus);
        Assert.Equal(("madt cpu 01", null, null), (extra.Key, extra.Field, extra.A));
        Assert.False(extra.Expected);
        Assert.Equal("only in B", extra.Why);

        var gone = One(HwDiff.Compare(Report(Qemu), With(("@hw\tmadt\tcpu", null))));
        Assert.Equal(("madt cpu 00", "00 00000001", null), (gone.Key, gone.A, gone.B));
        Assert.False(gone.Expected);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_machine_with_no_hypervisor_differs_from_a_VM_only_by_findings_about_the_machine()
    {
        var bare = With(("@hw\thypervisor", null), ("@hw\tcpuid\t40000000", null));
        var d = HwDiff.Compare(Report(Qemu), bare);
        Assert.Equal(2, d.Count);
        Assert.All(d, x => Assert.True(x.Expected, x.Key));

        // what bare metal prints: "hypervisor -", a signature and no max-leaf
        // (the rig's report, 1.8e: max-leaf was a finding)
        bare = With(("@hw\thypervisor", "@hw\thypervisor\t-"), ("@hw\tcpuid\t40000000", null));
        d = HwDiff.Compare(Report(Qemu), bare);
        Assert.Equal(3, d.Count);
        Assert.All(d, x => Assert.True(x.Expected, x.Key + " " + x.Field));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_report_says_what_is_wrong_with_its_own_machine()
    {
        string Note(params (string, string?)[] e) => Assert.Single(HwDiff.Notes(With(e)));

        Assert.Contains("by the PIT's count (999847680 Hz) and by the PM timer (59742000000 Hz)",
                        Note(("@hw\tclock\ttsc-pm-hz", "@hw\tclock\ttsc-pm-hz\t59742000000")));
        Assert.Contains("one clock is wrong", Note(("@hw\tclock\ttsc-pm-hz", "@hw\tclock\ttsc-pm-hz\t59742000000")));
        Assert.Contains("by the tick's interrupts", Note(("@hw\tclock\ttsc-pm-hz", "@hw\tclock\ttsc-pm-hz\t59742000000"),
                                                          ("@hw\tclock\ttsc-by", "@hw\tclock\ttsc-by\ttick"),
                                                          ("@hw\tclock\ttsc-pit-hz", "@hw\tclock\ttsc-pit-hz\t-")));
        // Hyper-V (1.8c): the clock as stated agrees with the PM timer; the PIT's count is 1.1% low
        Assert.Contains("by the PIT's count (988000000 Hz) and by the PM timer",
                        Note(("@hw\tclock\ttsc-by", "@hw\tclock\ttsc-by\thv"),
                             ("@hw\tclock\ttsc-hz", "@hw\tclock\ttsc-hz\t999994936"),
                             ("@hw\tclock\ttsc-pit-hz", "@hw\tclock\ttsc-pit-hz\t988000000")));
        Assert.Empty(HwDiff.Notes(With(("@hw\tclock\ttsc-by", "@hw\tclock\ttsc-by\thv"))));
        Assert.Contains("as the hypervisor states it (2000000000 Hz)",
                        Note(("@hw\tclock\ttsc-by", "@hw\tclock\ttsc-by\thv"),
                             ("@hw\tclock\ttsc-hz", "@hw\tclock\ttsc-hz\t2000000000")));
        Assert.Contains("came 64 a second for the 1000 asked", Note(("@hw\tclock\ttick-irq-hz", "@hw\tclock\ttick-irq-hz\t64")));
        Assert.Empty(HwDiff.Notes(With(("@hw\tclock\ttick-irq-hz", "@hw\tclock\ttick-irq-hz\t950"))));
        Assert.Empty(HwDiff.Notes(With(("@hw\tclock\ttick-irq-hz", "@hw\tclock\ttick-irq-hz\t-"))));
        Assert.Empty(HwDiff.Notes(With(("@hw\tclock\ttsc-pm-hz", "@hw\tclock\ttsc-pm-hz\t1005000000"))));
        Assert.Contains("not measured", Note(("@hw\tclock\ttsc-pm-hz", "@hw\tclock\ttsc-pm-hz\t-")));
        Assert.Contains("acpi rsdt SSDT #2 at 00000000bffe2500 is short",
                        Note(("@hw\tacpi\ttable\trsdt\tSSDT\t00000000bffe2500", "@hw\tacpi\ttable\trsdt\tSSDT\t00000000bffe2500\t00000010\t01\tBOCHS\tBXPC\t00000001\tshort")));
        Assert.Contains("RSDP's checksum is bad", Note(("@hw\tacpi\trsdp", "@hw\tacpi\trsdp\t00000000000f52c0\t00\tBOCHS\tbffe2328\t-\tbad\t-")));
        Assert.Contains("no RSDP", Note(("@hw\tacpi\trsdp", "@hw\tacpi\tnone\t0000000000000000"), ("@hw\tacpi\ttable", null)));
        Assert.Contains("offset 0000002c", Note(("@hw\tmadt\tnmi", "@hw\tmadt\tbad\t0000002c\t04\t00000003")));
        Assert.Contains("dropped 3", Note(("@hw\tmemsum", "@hw\tmemsum\t2\t3\t00000000bff7fc00")));
        Assert.Contains("PCI table is full", Note(("@hw\tpcisum", "@hw\tpcisum\t256\t256")));
        Assert.Contains("cut", Note(("@hw\tpcisum", "@hw\ttruncated")));
    }

    // ---- the report on disk (XCRL kind 6) ----

    /// <summary>A 4 MiB image whose XBPT table names a CRASH partition at sector 4096, as CrashLogTests builds one.</summary>
    private static byte[] Image()
    {
        var img = new byte[4 * 1024 * 1024];
        var t = img.AsSpan(512, 512);
        Encoding.ASCII.GetBytes("XBPT").CopyTo(t);
        BitConverter.TryWriteBytes(t[4..], (ushort)1);
        BitConverter.TryWriteBytes(t[6..], (ushort)1);
        BitConverter.TryWriteBytes(t[8..], (ushort)32);
        BitConverter.TryWriteBytes(t[12..], (ulong)(img.Length / 512));
        var e = img.AsSpan(512 + 32, 32);
        BitConverter.TryWriteBytes(e, 4096UL);
        BitConverter.TryWriteBytes(e[8..], 2048UL);
        e[16] = CXFlags.PART_CXCRASH;
        Encoding.ASCII.GetBytes("CRASH").CopyTo(e[20..]);
        return img;
    }

    private static void Put(byte[] img, int lba, ushort kind, byte[] text)
    {
        CrashLog.Seal(kind, text).CopyTo(img, (4096L + lba) * 512);
        text.CopyTo(img, (4096L + lba + 1) * 512);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_report_reads_back_from_its_own_sectors_and_a_torn_or_misplaced_one_is_refused()
    {
        var img = Image();
        Assert.Equal(CrashAreaState.Empty, CrashLog.ReadHwReport(new MemoryStream(img)).State);

        // Larger than a crash record may be: the report has its own cap.
        var body = Qemu.Concat(Enumerable.Range(0, 400).Select(i => $"@hw\tpci\t01:{i / 8:x2}.{i % 8}\t8086\t100e\t02\t00\t00\t00\t-\t-\t00\t00\t0\t0\t0\t0\t0\t0"));
        byte[] text = Encoding.ASCII.GetBytes(HwReport.Seal(body));
        Assert.True(text.Length > CrashLog.TextMax);
        Put(img, CrashLog.HwReportLba, CrashLog.KindHwReport, text);

        var r = CrashLog.ReadHwReport(new MemoryStream(img));
        Assert.Equal(CrashAreaState.Record, r.State);
        Assert.Equal(Encoding.ASCII.GetString(text), r.Text);
        Assert.Null(HwReport.Check(r.Text));
        // its neighbours' places are untouched
        Assert.Equal(CrashAreaState.Empty, CrashLog.Read(new MemoryStream(img)).State);
        Assert.Equal(CrashAreaState.Empty, CrashLog.ReadBootLog(new MemoryStream(img)).State);

        var torn = (byte[])img.Clone();
        torn[(4096 + CrashLog.HwReportLba + 1) * 512 + 100] ^= 1;
        Assert.Equal(CrashAreaState.Invalid, CrashLog.ReadHwReport(new MemoryStream(torn)).State);

        var boot = Image();
        Put(boot, CrashLog.HwReportLba, CrashLog.KindBootLog, "a system log\n"u8.ToArray());
        var wrong = CrashLog.ReadHwReport(new MemoryStream(boot));
        Assert.Equal(CrashAreaState.Invalid, wrong.State);
        Assert.Contains("kind 4", wrong.Detail);

        Assert.Throws<ArgumentException>(() => CrashLog.Seal(CrashLog.KindHwReport, new byte[CrashLog.HwReportMax + 1]));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_record_places_are_the_kernels()
    {
        string repo = Requires.Repo();
        string h = File.ReadAllText(Path.Combine(repo, "kernel", "diag", "crashrec.h"));
        long Define(string name)
        {
            var m = Regex.Match(h, $@"#define\s+{name}\s+\(?(\d+)u?(?:\s*\*\s*XCRL_SECTOR\))?", RegexOptions.Multiline);
            Assert.True(m.Success, $"{name} not found in crashrec.h");
            long v = long.Parse(m.Groups[1].Value);
            return m.Value.Contains("XCRL_SECTOR") ? v * 512 : v;
        }
        Assert.Equal(CrashLog.KindHwReport, Define("XCRL_KIND_HWREPORT"));
        Assert.Equal(CrashLog.HwReportLba, Define("XCRL_HWREPORT_LBA"));
        Assert.Equal(CrashLog.HwReportMax, Define("XCRL_HWREPORT_MAX"));
        Assert.Equal(CrashLog.KindBootLog, Define("XCRL_KIND_BOOTLOG"));
        Assert.Equal(CrashLog.BootLogLba, Define("XCRL_BOOTLOG_LBA"));
        Assert.Equal(CrashLog.BootLogMax, Define("XCRL_BOOTLOG_MAX"));
        Assert.Equal(CrashLog.SeedLba, Define("XCRL_SEED_LBA"));
        Assert.Equal(CrashLog.KindProfile, Define("XCRL_KIND_PROFILE"));
        Assert.Equal(CrashLog.ProfileLba, Define("XCRL_PROFILE_LBA"));
        Assert.Equal(CrashLog.ProfileMax, Define("XCRL_PROFILE_MAX"));
        // the places do not overlap, and all fit the partition the build makes (2048 sectors)
        Assert.True(CrashLog.HwReportLba + 1 + CrashLog.HwReportMax / 512 <= CrashLog.ProfileLba);
        Assert.True(CrashLog.ProfileLba + 1 + CrashLog.ProfileMax / 512 <= 2048);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Cxk_reads_the_report_off_a_disk_and_judges_two_by_exit_code()
    {
        string cxk = Requires.Cxk();
        string dir = Path.Combine(Path.GetTempPath(), "cxk-hw-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(dir);
        try
        {
            var img = Image();
            string text = HwReport.Seal(Qemu);
            Put(img, CrashLog.HwReportLba, CrashLog.KindHwReport, Encoding.ASCII.GetBytes(text));
            string image = Path.Combine(dir, "a.img"), saved = Path.Combine(dir, "a.hw.txt");
            File.WriteAllBytes(image, img);

            var rig = HostBuild.Run(cxk, ["rig", "hwreport", image, "-o", saved]);
            Assert.True(rig.ExitCode == 0, rig.StdOut + rig.StdErr);
            Assert.Equal(text, File.ReadAllText(saved));

            // a serial log: CR LF and other lines around the report
            string serial = Path.Combine(dir, "b.serial.log");
            var other = With(("@hw\tpci\t00:03.0", "@hw\tpci\t00:03.0\t8086\t10d3\t02\t00\t00\t00\t1af4\t1100\t0b\t01\tfebc0000\t0000c001\t00000000\t00000000\t00000000\t00000000"));
            File.WriteAllText(serial, "[OK  ] noise\r\n" + other.Text.Replace("\n", "\r\n\r\n") + "@ktest\tx\r\n");

            Assert.Equal(0, HostBuild.Run(cxk, ["hw", "diff", image, saved]).ExitCode);
            var diff = HostBuild.Run(cxk, ["hw", "diff", image, serial]);
            Assert.Equal(1, diff.ExitCode);
            Assert.Contains("100e -> 10d3", diff.StdOut);
            Assert.Equal(2, HostBuild.Run(cxk, ["hw", "diff", image, Path.Combine(dir, "missing.log")]).ExitCode);

            var show = HostBuild.Run(cxk, ["hw", "show", serial, "-s", "pci"]);
            Assert.True(show.ExitCode == 0, show.StdOut + show.StdErr);
            Assert.Contains("device=10d3", show.StdOut);
            Assert.DoesNotContain("cpuid", show.StdOut);
            // HostBuild.Run hands stdout back line by line, joined with the host's newline
            Assert.Equal(text.TrimEnd('\n'), HostBuild.Run(cxk, ["hw", "show", image, "--raw"]).StdOut.ReplaceLineEndings("\n").TrimEnd('\n'));

            // a whole XCRL record whose report is not whole: refused, not printed
            var torn = Image();
            Put(torn, CrashLog.HwReportLba, CrashLog.KindHwReport, Encoding.ASCII.GetBytes(text.Replace("\tbios\t", "\tuefi\t")));
            File.WriteAllBytes(Path.Combine(dir, "torn.img"), torn);
            var refused = HostBuild.Run(cxk, ["rig", "hwreport", Path.Combine(dir, "torn.img")]);
            Assert.Equal(1, refused.ExitCode);
            Assert.Contains("damaged", refused.StdOut);
            Assert.Equal(1, HostBuild.Run(cxk, ["hw", "show", Path.Combine(dir, "torn.img")]).ExitCode);

            Assert.Equal(1, HostBuild.Run(cxk, ["rig", "hwreport", Path.Combine(dir, "empty.img")]).ExitCode);   // not there yet
            File.WriteAllBytes(Path.Combine(dir, "empty.img"), Image());
            Assert.Equal(2, HostBuild.Run(cxk, ["rig", "hwreport", Path.Combine(dir, "empty.img")]).ExitCode);
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }

    /// <summary>A profile as the self-tests print it (1.9f): two CPUs, a TSC at 1 GHz,
    /// a stress window of 2e9 ticks.</summary>
    private static readonly string[] Profile =
    [
        "@hw\tprofile\t2\t1000000000\t2000000000\t400",
        "@hw\tlock\tsched\tboot\t0\t5000\t100\t900\t3000000\t40000",
        "@hw\tlock\tsched\tboot\t1\t3000\t50\t400\t1000000\t25000",
        "@hw\tlock\tsched\tstress\t0\t1000\t60\t500\t30000000\t-",
        "@hw\tlock\tsched\tstress\t1\t1000\t40\t300\t10000000\t-",
    ];

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_profile_is_told_from_the_machines_report_on_serial_and_each_is_found_alone()
    {
        string machine = HwReport.Seal(Qemu), profile = HwReport.Seal(Profile);
        string log = "noise\r\n" + machine.Replace("\n", "\r\n") + "[OK] self-tests\r\n" + profile.Replace("\n", "\r\n");
        Assert.Equal(machine, HwReport.TextFromSerial(log));
        Assert.Equal(profile, HwReport.ProfileFromSerial(log));
        Assert.Null(HwReport.ProfileFromSerial(machine));
        Assert.Null(HwReport.TextFromSerial(profile));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_profile_differs_only_by_measures_and_says_what_contention_cost()
    {
        var a = Report(Profile);
        var b = Report(Profile.Select(l => l.Replace("\t1000\t60\t", "\t1200\t90\t")));
        var diffs = HwDiff.Compare(a, b);
        Assert.Equal(2, diffs.Count);
        Assert.All(diffs, d => Assert.True(d.Expected, d.Key));
        Assert.Contains(diffs, d => d.Key == "lock sched stress cpu0" && d.Field == "contended" && d.A == "60" && d.B == "90");

        var notes = HwDiff.Contention(a);
        // boot: 150 of 8000 takes; 4 ms spinning; longest 40000 ticks = 40 µs
        Assert.Contains("lock sched boot: 1.875% of 8000 takes contended, 1300 spins, 4 ms spinning in all, longest hold 40 µs", notes);
        // stress: 100 of 2000; 4e7 of 2 CPUs x 2e9 ticks = 1%
        Assert.Contains("lock sched stress: 5% of 2000 takes contended, 800 spins, 1% of 2 CPUs' time spinning", notes);
        Assert.Empty(HwDiff.Contention(Report(Qemu)));
        Assert.Contains(notes[0], HwDiff.Notes(a));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_profile_reads_back_from_its_own_sectors_apart_from_the_report()
    {
        var img = Image();
        Assert.Equal(CrashAreaState.Empty, CrashLog.ReadProfile(new MemoryStream(img)).State);

        byte[] machine = Encoding.ASCII.GetBytes(HwReport.Seal(Qemu)), profile = Encoding.ASCII.GetBytes(HwReport.Seal(Profile));
        Put(img, CrashLog.HwReportLba, CrashLog.KindHwReport, machine);
        Assert.Equal(CrashAreaState.Empty, CrashLog.ReadProfile(new MemoryStream(img)).State);   // a report is not a profile
        Put(img, CrashLog.ProfileLba, CrashLog.KindProfile, profile);

        var p = CrashLog.ReadProfile(new MemoryStream(img));
        Assert.Equal(CrashAreaState.Record, p.State);
        Assert.Equal(Encoding.ASCII.GetString(profile), p.Text);
        Assert.Equal(Encoding.ASCII.GetString(machine), CrashLog.ReadHwReport(new MemoryStream(img)).Text);   // its neighbour untouched

        var torn = (byte[])img.Clone();
        torn[(4096 + CrashLog.ProfileLba + 1) * 512 + 20] ^= 1;
        Assert.Equal(CrashAreaState.Invalid, CrashLog.ReadProfile(new MemoryStream(torn)).State);

        var wrong = Image();
        Put(wrong, CrashLog.ProfileLba, CrashLog.KindHwReport, machine);
        var w = CrashLog.ReadProfile(new MemoryStream(wrong));
        Assert.Equal(CrashAreaState.Invalid, w.State);
        Assert.Contains("kind 6", w.Detail);

        Assert.Throws<ArgumentException>(() => CrashLog.Seal(CrashLog.KindProfile, new byte[CrashLog.ProfileMax + 1]));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Cxk_reads_the_profile_off_a_disk_as_off_serial()
    {
        string cxk = Requires.Cxk();
        string dir = Path.Combine(Path.GetTempPath(), "cxk-prof-" + Guid.NewGuid().ToString("N")[..8]);
        Directory.CreateDirectory(dir);
        try
        {
            string profile = HwReport.Seal(Profile);
            var img = Image();
            Put(img, CrashLog.HwReportLba, CrashLog.KindHwReport, Encoding.ASCII.GetBytes(HwReport.Seal(Qemu)));
            Put(img, CrashLog.ProfileLba, CrashLog.KindProfile, Encoding.ASCII.GetBytes(profile));
            string image = Path.Combine(dir, "a.img"), serial = Path.Combine(dir, "b.serial.log");
            File.WriteAllBytes(image, img);
            File.WriteAllText(serial, "[OK  ] noise\r\n" + HwReport.Seal(Qemu).Replace("\n", "\r\n") + "[OK] self-tests\r\n"
                + HwReport.Seal(Profile.Select(l => l.Replace("\t1000\t60\t", "\t1200\t90\t"))).Replace("\n", "\r\n"));

            var raw = HostBuild.Run(cxk, ["hw", "show", image, "--profile", "--raw"]);
            Assert.True(raw.ExitCode == 0, raw.StdOut + raw.StdErr);
            Assert.Equal(profile.TrimEnd('\n'), raw.StdOut.ReplaceLineEndings("\n").TrimEnd('\n'));

            // disk beside serial: the measures differ, which is no finding
            var diff = HostBuild.Run(cxk, ["hw", "diff", image, serial, "--profile"]);
            Assert.True(diff.ExitCode == 0, diff.StdOut + diff.StdErr);
            Assert.Contains("60 -> 90", diff.StdOut);

            // a disk that never booted a test-hook kernel says so
            File.WriteAllBytes(Path.Combine(dir, "none.img"), Image());
            var none = HostBuild.Run(cxk, ["hw", "show", Path.Combine(dir, "none.img"), "--profile"]);
            Assert.Equal(1, none.ExitCode);
            Assert.Contains("no profile saved", Regex.Replace(none.StdOut, @"\s+", " "));   // the console wraps it
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }}
