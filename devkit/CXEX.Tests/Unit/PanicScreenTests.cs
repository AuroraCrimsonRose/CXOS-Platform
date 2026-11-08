// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;
using Xunit;
using ZXing;
using ZXing.Common;
using ZXing.QrCode;

namespace CXEX.Tests.Unit;

/// <summary>
/// The panic screen's QR code, read by a real QR reader (ZXing.Net). The
/// kernel's own self-test pins the code's bits; only a decoder shows those bits
/// are a code anyone can read (roadmap Phase 2 step 3a).
/// </summary>
public class PanicScreenTests
{
    /// <summary>
    /// The fixed record, encoded by the kernel's own code compiled for the
    /// host (kernel/test/host/panic_qr_dump.c), at the smallest size the screen
    /// will draw: 3 pixels per module, a 4-module quiet zone.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Toolchain)]
    public void The_fixed_record_decodes_to_its_text()
    {
        var (repo, bin, extraPath) = KernelHostTests.Build();
        string exe = Path.Combine(bin, OperatingSystem.IsWindows() ? "panic_qr_dump.exe" : "panic_qr_dump");
        Assert.True(File.Exists(exe), $"the build reported success but {exe} does not exist");
        var run = KernelHostTests.Run(exe, Array.Empty<string>(), repo, extraPath);
        Assert.True(run.Code == 0, $"panic_qr_dump failed (exit {run.Code}):\n{run.Err}");

        string[] lines = run.Out.Replace("\r\n", "\n").Split('\n');
        int modules = int.Parse(lines[0]);
        Assert.Equal("text", lines[modules + 1]);
        string want = string.Join("\n", lines.Skip(modules + 2));

        const int Px = 3, Quiet = 4;
        int side = (modules + 2 * Quiet) * Px;
        var luma = new byte[side * side];
        Array.Fill(luma, (byte)255);
        for (int y = 0; y < modules; y++)
            for (int x = 0; x < modules; x++)
                if (lines[y + 1][x] == '1')
                    for (int py = 0; py < Px; py++)
                        for (int px = 0; px < Px; px++)
                            luma[((Quiet + y) * Px + py) * side + (Quiet + x) * Px + px] = 0;

        Assert.Equal(want, Decode(luma, side, side));
    }

    /// <summary>
    /// A real panic in QEMU, with the screen held as a display server would
    /// hold it and another CPU painting over it (panic_test_prepare), read
    /// from the screen: the code must be there, whole, and agree with the
    /// report the same panic sent to the serial port.
    /// </summary>
    [Fact]
    [Trait(Categories.Key, Categories.Toolchain)]
    public void A_forced_panic_shows_a_code_that_decodes()
    {
        string repo = Requires.Repo();
        string image = Path.Combine(repo, "dist", "CXK_x86_64", "images", "cxk_x86_64.img");
        if (!File.Exists(image)) Assert.Skip($"no built image at {image}: run `cxk os build` first");
        if (!TestEnv.HasTool("qemu-system-x86_64")) Assert.Skip("qemu-system-x86_64 is not on PATH");

        string dump = Path.Combine(Path.GetTempPath(), $"cxk-panic-{Guid.NewGuid():N}.ppm");
        try
        {
            var psi = new ProcessStartInfo(Requires.Cxk())
            {
                RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false,
            };
            foreach (string a in new[] { "verify-crashlog", image, "-t", "600", "--screendump", dump }) psi.ArgumentList.Add(a);
            using var p = Process.Start(psi)!;
            var stderr = p.StandardError.ReadToEndAsync();
            string stdout = p.StandardOutput.ReadToEnd();
            p.WaitForExit();
            Assert.True(p.ExitCode == 0, $"verify-crashlog failed (exit {p.ExitCode}):\n{stdout}\n{stderr.Result}");

            var (w, h, luma) = ReadPpm(dump);
            string text = Decode(luma, w, h);

            Assert.Contains("stop: KERNEL_CHECK_FAILED (CX-0100)", text);
            Assert.Contains("reason: deliberate panic: a crash-log test asked for one", text);
            Assert.Contains("trace:\n", text);
            Assert.Contains(" kmain_late+0x", text);
            Assert.True(Encoding.ASCII.GetByteCount(text) <= 997, "the text is over version 25's capacity");

            // The number on the code is the number the same panic reported.
            string serial = File.ReadAllText(image + ".crashtest1.serial.log").Replace("\r", "");
            var onSerial = Regex.Match(serial, @"crash number ([0-9A-F]{8})");
            Assert.True(onSerial.Success, "the serial log has no crash number");
            Assert.Contains("crash: " + onSerial.Groups[1].Value, text);
        }
        finally
        {
            if (File.Exists(dump)) File.Delete(dump);
        }
    }

    private static string Decode(byte[] luma, int width, int height)
    {
        var bitmap = new BinaryBitmap(new HybridBinarizer(new PlanarYUVLuminanceSource(luma, width, height, 0, 0, width, height, false)));
        var hints = new Dictionary<DecodeHintType, object> { [DecodeHintType.TRY_HARDER] = true };
        var result = new QRCodeReader().decode(bitmap, hints);
        Assert.True(result is not null, "no QR code could be read");
        return result!.Text;
    }

    /// <summary>A binary PPM, as QEMU's screendump writes one, as luminance.</summary>
    private static (int Width, int Height, byte[] Luma) ReadPpm(string path)
    {
        byte[] b = File.ReadAllBytes(path);
        int at = 0;
        string Token()
        {
            while (char.IsWhiteSpace((char)b[at])) at++;
            int s = at;
            while (!char.IsWhiteSpace((char)b[at])) at++;
            return Encoding.ASCII.GetString(b, s, at - s);
        }
        Assert.Equal("P6", Token());
        int w = int.Parse(Token()), h = int.Parse(Token());
        Assert.Equal("255", Token());
        at++;   // the one whitespace byte before the samples
        var luma = new byte[w * h];
        for (int i = 0; i < w * h; i++)
        {
            int p = at + i * 3;
            luma[i] = (byte)((b[p] * 299 + b[p + 1] * 587 + b[p + 2] * 114) / 1000);
        }
        return (w, h, luma);
    }
}
