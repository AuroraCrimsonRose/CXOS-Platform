// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Text.RegularExpressions;
using CXEX.Build.Emitters;
using CXEX.Build.Engines;
using CXEX.Tests;
using Xunit;

namespace CXEX.Tests.Unit;

/// <summary>
/// Stage 2 reads the kernel within a budget (<c>KERNEL_SECTORS</c> in
/// boot/stage2.asm) and the image writer refuses a kernel past it
/// (<see cref="XBPTImageWriter.Stage2KernelSectorBudget"/>). Two constants in two
/// languages: these tests hold them equal, and check that the writer patches the
/// kernel's real size into stage 2, so it reads the kernel and not the budget.
/// </summary>
public class Stage2BudgetTests
{
    private static int? AsmConstant(string name)
    {
        string? repo = TestEnv.RepoRoot;
        if (repo is null) return null;
        string path = Path.Combine(repo, "boot", "stage2.asm");
        if (!File.Exists(path)) return null;

        var m = Regex.Match(File.ReadAllText(path), $@"^{name}\s+equ\s+(\d+)\b", RegexOptions.Multiline);
        Assert.True(m.Success, $"boot/stage2.asm has no `{name} equ <number>`");
        return int.Parse(m.Groups[1].Value);
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_writers_budget_is_what_stage_2_reads()
    {
        int? sectors = AsmConstant("KERNEL_SECTORS");
        int? chunk = AsmConstant("KERNEL_CHUNK");
        if (sectors is null || chunk is null) { Assert.True(true, "no checkout: skipped"); return; }

        Assert.Equal(sectors.Value, XBPTImageWriter.Stage2KernelSectorBudget);
        Assert.Equal(chunk.Value, XBPTImageWriter.Stage2KernelChunk);
        Assert.Equal(0, sectors.Value % chunk.Value);   // the budget is whole chunks
    }

    private static byte[] Stage2WithMarker(int at)
    {
        var stage2 = new byte[4096];
        "TNSK"u8.CopyTo(stage2.AsSpan(at));
        BitConverter.GetBytes((uint)XBPTImageWriter.Stage2KernelSectorBudget).CopyTo(stage2, at + 4);
        return stage2;
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void The_kernels_size_is_patched_into_stage_2()
    {
        const int at = 1234;
        byte[] stage2 = Stage2WithMarker(at);
        byte[] kernel = new byte[600_001];                 // 1172 sectors, the last one partial
        var map = DiskGeometryEngine.CalculateLayout(64, 8, stage2.Length, 0);
        string path = Path.Combine(Path.GetTempPath(), $"cxk-stage2-{Guid.NewGuid():N}.img");
        try
        {
            XBPTImageWriter.WriteImage(path, map, new byte[512], stage2, kernel, new List<StagedFile>());
            byte[] image = File.ReadAllBytes(path);
            int off = (int)(map.Stage2Lba * 512) + at;
            Assert.Equal("TNSK"u8.ToArray(), image[off..(off + 4)]);
            Assert.Equal(1172u, BitConverter.ToUInt32(image, off + 4));
        }
        finally { File.Delete(path); }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_kernel_past_the_budget_is_refused()
    {
        byte[] stage2 = Stage2WithMarker(100);
        byte[] kernel = new byte[XBPTImageWriter.Stage2KernelSectorBudget * 512 + 1];
        var map = DiskGeometryEngine.CalculateLayout(64, 8, stage2.Length, 0);
        string path = Path.Combine(Path.GetTempPath(), $"cxk-stage2-{Guid.NewGuid():N}.img");
        try
        {
            var e = Assert.Throws<InvalidDataException>(() =>
                XBPTImageWriter.WriteImage(path, map, new byte[512], stage2, kernel, new List<StagedFile>()));
            Assert.Contains("beyond the", e.Message);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    [Trait(Categories.Key, Categories.Unit)]
    public void A_read_past_the_boot_partition_is_refused()
    {
        byte[] stage2 = Stage2WithMarker(100);
        byte[] kernel = new byte[1024 * 1024 + 1];          // 2049 sectors: 33 chunks
        var map = DiskGeometryEngine.CalculateLayout(64, 1, stage2.Length, 0);  // 2048 sectors
        string path = Path.Combine(Path.GetTempPath(), $"cxk-stage2-{Guid.NewGuid():N}.img");
        try
        {
            var e = Assert.Throws<InvalidDataException>(() =>
                XBPTImageWriter.WriteImage(path, map, new byte[512], stage2, kernel, new List<StagedFile>()));
            Assert.Contains("BOOT partition holds 2048", e.Message);
        }
        finally { File.Delete(path); }
    }
}
