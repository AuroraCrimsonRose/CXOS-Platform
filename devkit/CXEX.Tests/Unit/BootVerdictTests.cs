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
    private const string Summary = "[KTEST   ] - [OK  ] - self-tests: all 45 passed\n";
    private const string Refusal = "-> signature check failed - unsigned, or signed by a key this kernel does not trust\n";
    private const string Executive = "[EXEC    ] - [OK  ] - executive started, pid 1\n";
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
    public void Silence_is_inconclusive_never_a_pass()
    {
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify("", full: false).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_full_boot_also_needs_the_executive()
    {
        Assert.Equal(BootOutcome.Inconclusive, BootVerdict.Classify(Summary, full: true).Outcome);
        Assert.Equal(BootOutcome.Pass, BootVerdict.Classify(Summary + Executive, full: true).Outcome);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void Verification_boots_never_write_to_the_image()
    {
        // Booting a release image without -snapshot once broke its SHA256SUMS.
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log"));
        Assert.Contains("-snapshot", args);
        Assert.Contains("pc", args);   // q35 makes the ATA self-test vacuous
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_cpu_model_reaches_qemu_only_when_asked_for()
    {
        // QEMU's default model unless one is named; a named one as -cpu, so
        // the minimal and the full model boots really differ.
        var plain = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log"));
        Assert.DoesNotContain("-cpu", plain);
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log", "pentium"));
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
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log"));
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
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log", usb: host));
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
        var args = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log", foreignDisk: "f.img"));
        int foreign = args.IndexOf("format=raw,file=f.img,if=ide,index=0,snapshot=off");
        int boot = args.IndexOf("ide-hd,drive=cxkdisk,bus=ide.0,unit=1,bootindex=0");
        Assert.True(foreign >= 0 && boot > foreign);
        Assert.Contains("-snapshot", args);

        var usb = QemuTool.BuildArguments(BootVerifier.ConfigFor("cxk_disk.img", "serial.log", usb: UsbHost.Xhci, foreignDisk: "f.img"));
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
}
