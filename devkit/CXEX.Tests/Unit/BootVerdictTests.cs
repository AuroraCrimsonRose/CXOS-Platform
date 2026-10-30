// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Build.Emitters;
using CXEX.Tools;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// How <c>cxk verify-boot</c> reads a serial log. The lines are real ones from
/// boots on 2026-10-16. The rules came over from bootverify.py, which learned
/// them the hard way: a panic beats a summary, the self-tests' deliberate
/// refusals are not failures, and silence is never a pass.
/// </summary>
public class BootVerdictTests
{
    private const string SummaryLine = "[KTEST   ] - [OK  ] - self-tests: all 45 passed\n";
    // since 1.5d a summary needs the per-test lines behind it (SelfTestReport)
    private static readonly string Summary = Results(45, failAt: -1) + SummaryLine;

    /// <summary>The @ktest lines kernel/ktest.c writes for n tests, one failing at failAt.</summary>
    private static string Results(int n, int failAt, int endPassed = -1)
    {
        var s = new System.Text.StringBuilder("\r\n@ktest\tbegin\r\n");
        for (int i = 0; i < n; i++)
            s.Append($"\r\n@ktest\tresult\t{i}\t{(i == failAt ? "fail" : "pass")}\t{i % 7}\ttest number {i}\r\n");
        int passed = endPassed >= 0 ? endPassed : (failAt >= 0 ? n - 1 : n);
        s.Append($"\r\n@ktest\tend\t{passed}\t{n}\r\n");
        return s.ToString();
    }
    private const string Refusal = "-> signature check failed - unsigned, or signed by a key this kernel does not trust\n";
    private const string Executive = "[EXEC    ] - [OK  ] - executive started, pid 1\n";
    private const string Banner = "CXK executive online (.xoex) - brokering\n";
    private const string Prompt = "cxk> ";
    private const string Panic =
        "*** KERNEL PANIC ***\nInvariant violated: no esp0 stack for thread 0 (PROCESS_MODEL.md section 8)\n" +
        "Called from EIP: 0xC0100317\nSystem halted.";

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_summary_passes_and_carries_the_count()
    {
        var v = BootVerdict.Classify(Refusal + Summary, full: false);
        Assert.Equal(BootOutcome.Pass, v.Outcome);
        Assert.Equal(45, v.TestsPassed);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_panic_fails_even_after_a_summary()
    {
        var v = BootVerdict.Classify(Summary + Executive + Panic, full: true);
        Assert.Equal(BootOutcome.Fail, v.Outcome);
        Assert.Contains("no esp0 stack", v.Reason);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Failed_self_tests_fail()
    {
        var v = BootVerdict.Classify("[KTEST   ] - [FAIL] - esp0 follows the running thread\n" +
                                     "[KTEST   ] - [FAIL] - self-tests failed: 1 failure(s)\n", full: false);
        Assert.Equal(BootOutcome.Fail, v.Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_self_tests_deliberate_refusals_are_not_failures()
    {
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify(Refusal, full: false).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_summary_without_its_per_test_lines_fails()
    {
        // 1.5d: the summary is checked against the @ktest lines, so a kernel
        // that stopped writing them, or wrote them wrong, cannot pass
        var bare = BootVerdict.Classify(SummaryLine, full: false);
        Assert.Equal(BootOutcome.Fail, bare.Outcome);
        Assert.Contains("missing", bare.Reason);

        string noEnd = Results(45, -1).Replace("\r\n@ktest\tend\t45\t45\r\n", "");
        Assert.Contains("no end line", BootVerdict.Classify(noEnd + SummaryLine, full: false).Reason);

        var fewer = BootVerdict.Classify(Results(44, -1) + SummaryLine, full: false);
        Assert.Equal(BootOutcome.Fail, fewer.Outcome);
        Assert.Contains("44 of 44", fewer.Reason);

        // an end line that agrees, over results one of which failed
        var hidden = BootVerdict.Classify(Results(45, 3, endPassed: 45) + SummaryLine, full: false);
        Assert.Equal(BootOutcome.Fail, hidden.Outcome);
        Assert.Contains("test number 3", hidden.Reason);

        string skipped = Results(45, -1).Replace("\tresult\t7\t", "\tresult\t8\t");
        Assert.Contains("result 7 is numbered 8", BootVerdict.Classify(skipped + SummaryLine, full: false).Reason);

        // a retired x86-32 image never wrote them
        Assert.Equal(BootOutcome.Pass, BootVerdict.Classify(SummaryLine, full: false, results: false).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_failed_self_test_is_named_with_what_it_logged()
    {
        const string log =
            "\r\n@ktest\tbegin\r\n" +
            "\r\n@ktest\tresult\t0\tpass\t1\tpaging maps\r\n" +
            "[KTEST   ] - [ERR ] - futex: ended 1\r\n" +
            "[KTEST   ] - [INFO] - not a problem\r\n" +
            "[KTEST   ] - [ERR ] - futex failed at step 1\r\n" +
            "[KTEST   ] - [FAIL] - futexes and std/sync, from disk\r\n" +
            // a line another thread was halfway through when the result came
            "[PROC    ] - [INFO] - half a li\r\n@ktest\tresult\t1\tfail\t5040\tfutexes and std/sync, from disk\r\nne\r\n" +
            "\r\n@ktest\tresult\t2\tpass\t0\tCR0.WP\r\n" +
            "\r\n@ktest\tend\t2\t3\r\n" +
            "[KTEST   ] - [FAIL] - self-tests failed: 1 failure(s)\r\n";
        var v = BootVerdict.Classify(log, full: false);
        Assert.Equal(BootOutcome.Fail, v.Outcome);
        Assert.EndsWith("futexes and std/sync, from disk", v.Reason);

        var r = SelfTestReport.Parse(log);
        Assert.Equal(3, r.Results.Count);
        Assert.Equal(1, r.Failed);
        var f = r.Results[1];
        Assert.False(f.Passed);
        Assert.Equal(5040, f.Ms);
        Assert.Equal(["futex: ended 1", "futex failed at step 1"], f.Detail);
        Assert.Empty(r.Results[0].Detail);
        Assert.Equal((2, 3), (r.EndPassed, r.EndTotal));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_json_report_carries_every_result()
    {
        var log = Results(3, failAt: 1) + "[KTEST   ] - [FAIL] - self-tests failed: 1 failure(s)\n";
        var v = BootVerdict.Classify(log, full: false);
        using var doc = System.Text.Json.JsonDocument.Parse(
            SelfTestReport.Json(v, SelfTestReport.Parse(log), "a.img", "a.log", full: false, cpu: null));
        var root = doc.RootElement;
        Assert.Equal("fail", root.GetProperty("outcome").GetString());
        Assert.Equal(1, root.GetProperty("exitCode").GetInt32());
        var t = root.GetProperty("selfTests");
        Assert.Equal(3, t.GetProperty("total").GetInt32());
        Assert.Equal(1, t.GetProperty("failed").GetInt32());
        var results = t.GetProperty("results");
        Assert.Equal(3, results.GetArrayLength());
        Assert.False(results[1].GetProperty("passed").GetBoolean());
        Assert.Equal("test number 1", results[1].GetProperty("name").GetString());
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Silence_is_inconclusive_never_a_pass()
    {
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify("", full: false).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_full_boot_also_needs_the_executive()
    {
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify(Summary, full: true).Outcome);
        Assert.Equal(BootOutcome.Pass, BootVerdict.Classify(Summary + Executive + Banner + Prompt, full: true).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_full_boot_needs_ring_3_output_not_just_a_loaded_executive()
    {
        // "Started" is logged once the image is loaded. A broken x86-64
        // syscall wrapper (2026-10-28) lost the banner and still passed.
        var loaded = BootVerdict.Classify(Summary + Executive, full: true);
        Assert.Equal(BootOutcome.Inconclusive, loaded.Outcome);
        Assert.Contains("banner", loaded.Reason);

        var noShell = BootVerdict.Classify(Summary + Executive + Banner, full: true);
        Assert.Equal(BootOutcome.Inconclusive, noShell.Outcome);
        Assert.Contains("prompt", noShell.Reason);

        // the prompt alone is not the executive's banner
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify(Summary + Executive + Prompt, full: true).Outcome);
        // and none of it is needed when the boot is not a full one
        Assert.Equal(BootOutcome.Pass, BootVerdict.Classify(Summary + Executive, full: false).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void An_executive_refused_at_load_fails_a_full_boot()
    {
        // The lines from a boot whose executive carried a wrong ABI hash
        // (2026-10-26). kmain's "launched" is only a pid; it once said
        // "started", and this boot passed.
        const string log = "[EXEC    ] - [INFO] - executive launched, pid 1\n" +
                           "[PROC    ] - [ERR ] - image was built against a different ABI definition (hash mismatch); rebuild it\n" +
                           "[EXEC    ] - [ERR ] - executive refused at load\n";
        var v = BootVerdict.Classify(Summary + log, full: true);
        Assert.Equal(BootOutcome.Fail, v.Outcome);
        Assert.Contains("refused at load", v.Reason);
        Assert.Equal(BootOutcome.Pass, BootVerdict.Classify(Summary + log, full: false).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Verification_boots_never_write_to_the_image()
    {
        // Booting a release image without -snapshot once broke its SHA256SUMS.
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log"));
        Assert.Contains("-snapshot", args);
        Assert.Contains("pc", args);   // q35 makes the ATA self-test vacuous
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_cpu_model_reaches_qemu_only_when_asked_for()
    {
        // QEMU's default model unless one is named; a named one as -cpu, so
        // the minimal and the full model boots really differ.
        var plain = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log"));
        Assert.DoesNotContain("-cpu", plain);
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log", "pentium"));
        int i = args.IndexOf("-cpu");
        Assert.True(i >= 0 && i + 1 < args.Count);
        Assert.Equal("pentium", args[i + 1]);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Verification_boots_keep_guest_time_off_the_host_clock()
    {
        // Headless QEMU's host timers are coarsened by Windows 11, which made
        // the cycle-counter self-test fail on a good tree (QemuConfig.ICountNoSleep).
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log"));
        int i = args.IndexOf("-icount");
        Assert.True(i >= 0 && i + 1 < args.Count);
        Assert.Equal("shift=6,sleep=off", args[i + 1]);
    }

    [Theory]
    [Trait(Categories.Key, Categories.Unit)]
    [InlineData(UsbHost.Xhci, "qemu-xhci")]
    [InlineData(UsbHost.Ehci, "usb-ehci")]
    [InlineData(UsbHost.Ohci, "pci-ohci")]
    public void A_usb_boot_puts_the_image_on_a_stick_and_nothing_on_ide(UsbHost host, string device)
    {
        // As the rig boots: the stick is the only disk, so a crash log or a
        // self-test that passes has passed on USB, not on an IDE disk beside it.
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log", usb: host));
        Assert.Contains("usb-storage,bus=usbhc.0,drive=cxkdisk,bootindex=0", args);
        // The stick's controller is not the first of its kind: a kernel that
        // drives only the first controller must fail this boot, as on the rig.
        int empty = args.IndexOf($"{device},id=usbhc_empty");
        int stick = args.IndexOf($"{device},id=usbhc");
        Assert.True(empty >= 0 && stick > empty);
        Assert.DoesNotContain(args, a => a.Contains("if=ide"));
        Assert.Contains("-snapshot", args);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_foreign_disk_comes_first_and_keeps_what_the_guest_writes()
    {
        // The kernel must meet it before its own disk, as a machine's drive,
        // and -snapshot must not hide a write to it.
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log", foreignDisk: "f.img"));
        int foreign = args.IndexOf("format=raw,file=f.img,if=ide,index=0,snapshot=off");
        int boot = args.IndexOf("ide-hd,drive=cxkdisk,bus=ide.0,unit=1,bootindex=0");
        Assert.True(foreign >= 0 && boot > foreign);
        Assert.Contains("-snapshot", args);

        var usb = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log", usb: UsbHost.Xhci, foreignDisk: "f.img"));
        Assert.Contains("format=raw,file=f.img,if=ide,index=0,snapshot=off", usb);
        Assert.Contains("usb-storage,bus=usbhc.0,drive=cxkdisk,bootindex=0", usb);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_foreign_disk_is_the_image_under_another_id_and_any_change_is_seen()
    {
        // a stand-in image: an XBPT header at LBA 1 carrying a stamped disk_id
        var image = new byte[64 * 1024];
        "XBPT"u8.CopyTo(image.AsSpan(512));
        image[1000] = 0x5A;
        XBPTImageWriter.StampDiskId(image, 512);
        ulong id = XBPTImageWriter.DiskId(image, 512);
        Assert.Equal(512 + XBPTImageWriter.DiskIdOffset, ForeignDisk.DiskIdAt);

        string path = Path.Combine(Path.GetTempPath(), $"cxk-foreign-{Guid.NewGuid():N}.img");
        try
        {
            byte[] hash = ForeignDisk.Create(path, image);
            byte[] disk = File.ReadAllBytes(path);
            Assert.Equal(image.Length + ForeignDisk.PadBytes, disk.Length);
            // the same CXOS, but not the same disk
            Assert.Equal(image[1000], disk[1000]);
            Assert.True(disk.AsSpan(0, 512 + 20).SequenceEqual(image.AsSpan(0, 512 + 20)));
            ulong other = XBPTImageWriter.DiskId(disk, 512);
            Assert.NotEqual(0UL, other);
            Assert.NotEqual(id, other);
            Assert.Contains(disk.Skip(image.Length).Take(4096), b => b != 0);   // zeros would hide a write of zeros

            // read back while still held open for writing, as QEMU may hold it
            using (var f = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite))
            {
                Assert.True(ForeignDisk.Unchanged(path, hash));
                f.Position = disk.Length - 1;
                f.WriteByte((byte)~disk[^1]);
                f.Flush();
                Assert.False(ForeignDisk.Unchanged(path, hash));
            }
        }
        finally { File.Delete(path); }
        Assert.Null(ForeignDisk.Unchanged(path, [0]));   // gone: no verdict, not a pass

        // Seen by its size, the one the kernel logs for each disk it refuses
        long size = image.Length + ForeignDisk.PadBytes;
        Assert.True(ForeignDisk.Seen($"[STORAGE ] - [INFO] - left read-only, sectors: {size / 512}\n", size));
        Assert.False(ForeignDisk.Seen($"[STORAGE ] - [INFO] - left read-only, sectors: {size / 512}0\n", size));
        Assert.False(ForeignDisk.Seen("[STORAGE ] - [OK  ] - online, disks: 2\n", size));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_disk_id_is_a_hash_of_the_image_and_never_zero()
    {
        var a = new byte[8192];
        a[4000] = 1;
        var b = (byte[])a.Clone();
        XBPTImageWriter.StampDiskId(a, 512);
        XBPTImageWriter.StampDiskId(b, 512);
        Assert.Equal(XBPTImageWriter.DiskId(a, 512), XBPTImageWriter.DiskId(b, 512));   // reproducible
        Assert.NotEqual(0UL, XBPTImageWriter.DiskId(a, 512));

        XBPTImageWriter.StampDiskId(a, 512);   // stamping again hashes with the field cleared
        Assert.Equal(XBPTImageWriter.DiskId(b, 512), XBPTImageWriter.DiskId(a, 512));

        b[4000] = 2;
        XBPTImageWriter.StampDiskId(b, 512);
        Assert.NotEqual(XBPTImageWriter.DiskId(a, 512), XBPTImageWriter.DiskId(b, 512));   // another image, another id
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Only_the_three_host_controllers_parse()
    {
        Assert.Equal(UsbHost.Xhci, QemuTool.ParseUsbHost("XHCI"));
        Assert.Equal(UsbHost.Ehci, QemuTool.ParseUsbHost("ehci"));
        Assert.Equal(UsbHost.Ohci, QemuTool.ParseUsbHost("ohci"));
        Assert.Null(QemuTool.ParseUsbHost("uhci"));
        Assert.Null(QemuTool.ParseUsbHost(""));
        Assert.Null(QemuTool.ParseUsbHost(null));
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_default_boot_is_the_64_bit_qemu_and_x86_32_names_the_32_bit_one()
    {
        // The system is x86-64 (roadmap step 6d3b): a verification boot needs
        // no --arch. x86_32 still boots an image from the retired 32-bit line.
        string plain = QemuTool.CommandLine(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log"));
        Assert.StartsWith("qemu-system-x86_64 ", plain);
        // RAM past 4 GB, so the page allocator's high zone exists
        Assert.Contains($"-m {BootVerifier.X86_64MemoryMb}M ", plain);
        Assert.True(BootVerifier.X86_64MemoryMb > 4096);

        string narrow = QemuTool.CommandLine(BootVerifier.ConfigFor("cxk_x86_64.img", "serial.log", arch: QemuArch.X86_32));
        Assert.StartsWith("qemu-system-i386 ", narrow);
        // the same verification boot otherwise: snapshot, headless, icount
        Assert.Equal(plain.Replace($"-m {BootVerifier.X86_64MemoryMb}M ", "-m 256M ")["qemu-system-x86_64".Length..],
                     narrow["qemu-system-i386".Length..]);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Only_the_two_architectures_parse()
    {
        Assert.Equal(QemuArch.X86_32, QemuTool.ParseArch("x86_32"));
        Assert.Equal(QemuArch.X86_64, QemuTool.ParseArch("X86_64"));
        Assert.Null(QemuTool.ParseArch("amd64"));
        Assert.Null(QemuTool.ParseArch("i386"));
        Assert.Null(QemuTool.ParseArch(""));
        Assert.Null(QemuTool.ParseArch(null));
    }
}
