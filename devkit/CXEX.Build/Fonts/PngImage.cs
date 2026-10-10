// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.IO;
using System.IO.Compression;

namespace CXEX.Build.Fonts;

/// <summary>
/// Just enough PNG to read a BMFont glyph atlas: 8-bit, non-interlaced, colour
/// type 6 (RGBA) or 0 (greyscale). Anything else is refused by name rather than
/// guessed at.
///
/// This is deliberately not a general PNG decoder. It exists so the kernel does
/// not need one - see XfntFormat - and the narrower it is, the less there is to
/// get wrong on the one side where a bug is still only a build failure.
/// </summary>
public sealed class PngImage
{
    public int Width { get; }
    public int Height { get; }
    /// <summary>Row-major, 4 bytes per pixel, RGBA.</summary>
    public byte[] Rgba { get; }

    private PngImage(int w, int h, byte[] rgba) { Width = w; Height = h; Rgba = rgba; }

    private static readonly byte[] Signature = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

    public byte Alpha(int x, int y) => Rgba[(y * Width + x) * 4 + 3];

    /// <summary>
    /// The brightest colour channel. Ink is white-on-black in an atlas that
    /// carries its shape in colour, and taking the max rather than a weighted
    /// luminance keeps a coloured glyph from reading as half-lit.
    /// </summary>
    public byte Lum(int x, int y)
    {
        int o = (y * Width + x) * 4;
        byte r = Rgba[o], g = Rgba[o + 1], b = Rgba[o + 2];
        byte m = r > g ? r : g;
        return m > b ? m : b;
    }

    /// <summary>
    /// Is there ink at this pixel?
    ///
    /// Two export styles have to be told apart, and the difference is not
    /// something the descriptor can be trusted about. BMFont's `alphaChnl=1`
    /// says coverage is in the alpha channel, with RGB left at zero. But an
    /// atlas can equally be saved with the shape in the colour channels and the
    /// glyph box fully opaque - which is what these fonts are - and then alpha
    /// is 255 across every glyph and alone would mark every pixel as ink.
    ///
    /// In colour mode alpha is still required, so a white-but-transparent
    /// pixel is not ink; in alpha mode the colour channels are ignored, because
    /// they are legitimately zero there.
    /// </summary>
    public bool Ink(int x, int y, bool useAlpha, int threshold, bool darkInk = false)
        => useAlpha
            ? Alpha(x, y) >= threshold
            : Alpha(x, y) >= threshold &&
              (darkInk ? Lum(x, y) < threshold : Lum(x, y) >= threshold);

    public static PngImage Load(string path) => Parse(File.ReadAllBytes(path));

    public static PngImage Parse(byte[] file)
    {
        if (file.Length < 8) throw new InvalidDataException("not a PNG: too short");
        for (int i = 0; i < 8; i++)
            if (file[i] != Signature[i]) throw new InvalidDataException("not a PNG: bad signature");

        int width = 0, height = 0, bitDepth = 0, colorType = -1;
        bool sawIhdr = false;
        using var idat = new MemoryStream();

        int pos = 8;
        while (pos + 8 <= file.Length)
        {
            int len = ReadBe32(file, pos);
            if (len < 0) throw new InvalidDataException("chunk length overflows a signed int");
            string type = System.Text.Encoding.ASCII.GetString(file, pos + 4, 4);
            int dataAt = pos + 8;
            // length + type + data + crc, every term checked against the real
            // file length before it is used as an index
            if ((long)dataAt + len + 4 > file.Length)
                throw new InvalidDataException($"chunk '{type}' runs past the end of the file");

            switch (type)
            {
                case "IHDR":
                    if (len != 13) throw new InvalidDataException("IHDR is not 13 bytes");
                    width = ReadBe32(file, dataAt);
                    height = ReadBe32(file, dataAt + 4);
                    bitDepth = file[dataAt + 8];
                    colorType = file[dataAt + 9];
                    if (file[dataAt + 10] != 0) throw new InvalidDataException("unsupported compression method");
                    if (file[dataAt + 11] != 0) throw new InvalidDataException("unsupported filter method");
                    if (file[dataAt + 12] != 0) throw new InvalidDataException("interlaced PNG is not supported");
                    sawIhdr = true;
                    break;
                case "IDAT":
                    idat.Write(file, dataAt, len);
                    break;
                case "IEND":
                    pos = file.Length;     // stop
                    break;
            }
            if (pos == file.Length) break;
            pos = dataAt + len + 4;
        }

        if (!sawIhdr) throw new InvalidDataException("no IHDR");
        if (width <= 0 || height <= 0) throw new InvalidDataException($"bad dimensions {width}x{height}");
        if (bitDepth != 8) throw new InvalidDataException($"only 8-bit PNG is supported, got {bitDepth}-bit");
        if (colorType != 6 && colorType != 0)
            throw new InvalidDataException($"only colour type 0 (grey) or 6 (RGBA) is supported, got {colorType}");
        if ((long)width * height > 64L * 1024 * 1024)
            throw new InvalidDataException("image is implausibly large");

        int channels = colorType == 6 ? 4 : 1;
        int stride = width * channels;

        idat.Position = 0;
        var raw = new byte[(long)(stride + 1) * height];
        using (var z = new ZLibStream(idat, CompressionMode.Decompress))
        {
            int got = 0;
            while (got < raw.Length)
            {
                int n = z.Read(raw, got, raw.Length - got);
                if (n <= 0) break;
                got += n;
            }
            if (got != raw.Length)
                throw new InvalidDataException($"IDAT inflated to {got} bytes, expected {raw.Length}");
        }

        // Undo the per-scanline filters in place into a flat RGBA buffer.
        var cur = new byte[stride];
        var prev = new byte[stride];
        var rgba = new byte[(long)width * height * 4];

        for (int y = 0; y < height; y++)
        {
            int rowAt = y * (stride + 1);
            int filter = raw[rowAt];
            Buffer.BlockCopy(raw, rowAt + 1, cur, 0, stride);
            Unfilter(filter, cur, prev, channels);

            for (int x = 0; x < width; x++)
            {
                int o = (y * width + x) * 4;
                if (channels == 4)
                {
                    rgba[o + 0] = cur[x * 4 + 0];
                    rgba[o + 1] = cur[x * 4 + 1];
                    rgba[o + 2] = cur[x * 4 + 2];
                    rgba[o + 3] = cur[x * 4 + 3];
                }
                else
                {
                    byte g = cur[x];
                    rgba[o + 0] = g; rgba[o + 1] = g; rgba[o + 2] = g; rgba[o + 3] = 255;
                }
            }
            (prev, cur) = (cur, prev);
        }

        return new PngImage(width, height, rgba);
    }

    private static void Unfilter(int filter, byte[] cur, byte[] prev, int bpp)
    {
        switch (filter)
        {
            case 0: break;
            case 1:
                for (int i = bpp; i < cur.Length; i++) cur[i] = (byte)(cur[i] + cur[i - bpp]);
                break;
            case 2:
                for (int i = 0; i < cur.Length; i++) cur[i] = (byte)(cur[i] + prev[i]);
                break;
            case 3:
                for (int i = 0; i < cur.Length; i++)
                {
                    int left = i >= bpp ? cur[i - bpp] : 0;
                    cur[i] = (byte)(cur[i] + ((left + prev[i]) >> 1));
                }
                break;
            case 4:
                for (int i = 0; i < cur.Length; i++)
                {
                    int a = i >= bpp ? cur[i - bpp] : 0;
                    int b = prev[i];
                    int c = i >= bpp ? prev[i - bpp] : 0;
                    cur[i] = (byte)(cur[i] + Paeth(a, b, c));
                }
                break;
            default:
                throw new InvalidDataException($"unknown scanline filter {filter}");
        }
    }

    private static int Paeth(int a, int b, int c)
    {
        int p = a + b - c;
        int pa = Math.Abs(p - a), pb = Math.Abs(p - b), pc = Math.Abs(p - c);
        return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
    }

    private static int ReadBe32(byte[] b, int at)
        => (b[at] << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3];
}
