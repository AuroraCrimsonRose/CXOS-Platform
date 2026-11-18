// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.IO;

namespace CXEX.Build.Pictures;

/// <summary>
/// QOI, "the Quite OK Image format" (qoiformat.org, specification 1.0), RGB
/// only. Lossless, and a reader is six cases in one loop, which is why the OS
/// reads pictures in it (std/picture.xfxn) rather than in anything it would
/// need a real codec for. Written from the published specification.
///
/// A stream is a 14-byte header ("qoif", width and height big-endian,
/// channels, colour space), then chunks, then seven 0x00 and one 0x01.
/// </summary>
public static class Qoi
{
    public const int HeaderSize = 14;
    public static readonly byte[] End = { 0, 0, 0, 0, 0, 0, 0, 1 };

    private const byte OpIndex = 0x00, OpDiff = 0x40, OpLuma = 0x80, OpRun = 0xC0, OpRgb = 0xFE, OpRgba = 0xFF;

    private static int Hash(int r, int g, int b, int a) => (r * 3 + g * 5 + b * 7 + a * 11) % 64;

    public static byte[] Encode(Picture pic)
    {
        var o = new MemoryStream(pic.Width * pic.Height + 64);
        o.Write("qoif"u8);
        void U32(int v) { o.WriteByte((byte)(v >> 24)); o.WriteByte((byte)(v >> 16)); o.WriteByte((byte)(v >> 8)); o.WriteByte((byte)v); }
        U32(pic.Width); U32(pic.Height);
        o.WriteByte(3); o.WriteByte(0);

        var index = new int[64];       // packed 0xRRGGBB, alpha always 255
        Array.Fill(index, -1);
        int pr = 0, pg = 0, pb = 0, run = 0;
        int n = pic.Width * pic.Height;
        byte[] px = pic.Rgb;
        for (int i = 0; i < n; i++)
        {
            int r = px[i * 3], g = px[i * 3 + 1], b = px[i * 3 + 2];
            if (r == pr && g == pg && b == pb)
            {
                run++;
                if (run == 62 || i == n - 1) { o.WriteByte((byte)(OpRun | (run - 1))); run = 0; }
                continue;
            }
            if (run > 0) { o.WriteByte((byte)(OpRun | (run - 1))); run = 0; }
            int h = Hash(r, g, b, 255), packed = (r << 16) | (g << 8) | b;
            if (index[h] == packed)
            {
                o.WriteByte((byte)(OpIndex | h));
            }
            else
            {
                index[h] = packed;
                int dr = (sbyte)(r - pr), dg = (sbyte)(g - pg), db = (sbyte)(b - pb);
                int drg = dr - dg, dbg = db - dg;
                if (dr is >= -2 and <= 1 && dg is >= -2 and <= 1 && db is >= -2 and <= 1)
                    o.WriteByte((byte)(OpDiff | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
                else if (dg is >= -32 and <= 31 && drg is >= -8 and <= 7 && dbg is >= -8 and <= 7)
                {
                    o.WriteByte((byte)(OpLuma | (dg + 32)));
                    o.WriteByte((byte)(((drg + 8) << 4) | (dbg + 8)));
                }
                else
                {
                    o.WriteByte(OpRgb); o.WriteByte((byte)r); o.WriteByte((byte)g); o.WriteByte((byte)b);
                }
            }
            pr = r; pg = g; pb = b;
        }
        o.Write(End);
        return o.ToArray();
    }

    /// <summary>Reads RGB or RGBA QOI (alpha dropped), checking every bound, as the OS's reader does.</summary>
    public static Picture Decode(byte[] d)
    {
        if (d.Length < HeaderSize + End.Length || d[0] != 'q' || d[1] != 'o' || d[2] != 'i' || d[3] != 'f')
            throw new InvalidDataException("not QOI");
        long w = ((long)d[4] << 24) | ((long)d[5] << 16) | ((long)d[6] << 8) | d[7];
        long h = ((long)d[8] << 24) | ((long)d[9] << 16) | ((long)d[10] << 8) | d[11];
        if (w == 0 || h == 0 || w > 16384 || h > 16384 || (d[12] != 3 && d[12] != 4) || d[13] > 1)
            throw new InvalidDataException("a QOI header out of range");
        int n = (int)(w * h);
        var rgb = new byte[n * 3];
        var index = new uint[64];
        uint r = 0, g = 0, b = 0, a = 255;
        int p = HeaderSize, limit = d.Length - End.Length, run = 0;
        for (int i = 0; i < n; i++)
        {
            if (run > 0) run--;
            else
            {
                if (p >= limit) throw new InvalidDataException("QOI data ends before its pixels");
                byte op = d[p++];
                if (op == OpRgb)
                {
                    if (p + 3 > limit) throw new InvalidDataException("QOI data ends inside a chunk");
                    r = d[p]; g = d[p + 1]; b = d[p + 2]; p += 3;
                }
                else if (op == OpRgba)
                {
                    if (p + 4 > limit) throw new InvalidDataException("QOI data ends inside a chunk");
                    r = d[p]; g = d[p + 1]; b = d[p + 2]; a = d[p + 3]; p += 4;
                }
                else if ((op & 0xC0) == OpIndex)
                {
                    uint v = index[op];
                    r = (v >> 24) & 255; g = (v >> 16) & 255; b = (v >> 8) & 255; a = v & 255;
                }
                else if ((op & 0xC0) == OpDiff)
                {
                    r = (r + (uint)((op >> 4) & 3) - 2) & 255;
                    g = (g + (uint)((op >> 2) & 3) - 2) & 255;
                    b = (b + (uint)(op & 3) - 2) & 255;
                }
                else if ((op & 0xC0) == OpLuma)
                {
                    if (p + 1 > limit) throw new InvalidDataException("QOI data ends inside a chunk");
                    int dg = (op & 63) - 32, x = d[p++];
                    r = (uint)(r + dg - 8 + ((x >> 4) & 15)) & 255;
                    g = (uint)(g + dg) & 255;
                    b = (uint)(b + dg - 8 + (x & 15)) & 255;
                }
                else run = op & 63;
                index[Hash((int)r, (int)g, (int)b, (int)a)] = (r << 24) | (g << 16) | (b << 8) | a;
            }
            rgb[i * 3] = (byte)r; rgb[i * 3 + 1] = (byte)g; rgb[i * 3 + 2] = (byte)b;
        }
        for (int k = 0; k < End.Length; k++)
            if (d[limit + k] != End[k]) throw new InvalidDataException("QOI end marker missing");
        return new Picture((int)w, (int)h, rgb);
    }
}

/// <summary>Resampling for pictures that become a screen's background.</summary>
public static class PictureScale
{
    /// <summary>
    /// The size a picture is stored at so that it still covers a screen of
    /// w x h without being enlarged: scaled down, aspect kept, until one side
    /// meets the screen's. Never larger than the source.
    /// </summary>
    public static (int W, int H) CoverSize(int srcW, int srcH, int w, int h)
    {
        double s = Math.Max((double)w / srcW, (double)h / srcH);
        if (s >= 1) return (srcW, srcH);
        return (Math.Max(w, (int)Math.Round(srcW * s)), Math.Max(h, (int)Math.Round(srcH * s)));
    }

    /// <summary>
    /// Area-average resampling to w x h: each output pixel is the mean of the
    /// source pixels it covers, partial ones weighted by how much. Downscaling
    /// only; the aspect is the caller's business.
    /// </summary>
    public static Picture Resize(Picture src, int w, int h)
    {
        if (w <= 0 || h <= 0 || w > src.Width || h > src.Height) throw new ArgumentException("downscaling only");
        if (w == src.Width && h == src.Height) return src;
        // Horizontal pass into floats, then vertical.
        var mid = new float[w * src.Height * 3];
        var (hx0, hx1, hw) = Spans(src.Width, w);
        for (int y = 0; y < src.Height; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 3; c++)
                {
                    float s = 0;
                    for (int k = hx0[x]; k <= hx1[x]; k++) s += src.Rgb[(y * src.Width + k) * 3 + c] * hw[x][k - hx0[x]];
                    mid[(y * w + x) * 3 + c] = s;
                }
        var (vy0, vy1, vw) = Spans(src.Height, h);
        var rgb = new byte[w * h * 3];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 3; c++)
                {
                    float s = 0;
                    for (int k = vy0[y]; k <= vy1[y]; k++) s += mid[(k * w + x) * 3 + c] * vw[y][k - vy0[y]];
                    rgb[(y * w + x) * 3 + c] = (byte)Math.Clamp((int)MathF.Round(s), 0, 255);
                }
        return new Picture(w, h, rgb);
    }

    // For each output index: the first and last source index it covers and their weights (summing to 1).
    private static (int[] First, int[] Last, float[][] Weights) Spans(int from, int to)
    {
        var first = new int[to]; var last = new int[to]; var weights = new float[to][];
        double scale = (double)from / to;
        for (int i = 0; i < to; i++)
        {
            double a = i * scale, b = (i + 1) * scale;
            int k0 = (int)Math.Floor(a), k1 = Math.Min(from - 1, (int)Math.Ceiling(b) - 1);
            first[i] = k0; last[i] = k1;
            var wts = new float[k1 - k0 + 1];
            for (int k = k0; k <= k1; k++)
                wts[k - k0] = (float)((Math.Min(b, k + 1) - Math.Max(a, k)) / scale);
            weights[i] = wts;
        }
        return (first, last, weights);
    }
}
