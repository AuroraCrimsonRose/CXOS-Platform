// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.IO;

namespace CXEX.Build.Pictures;

/// <summary>
/// A picture as 8-bit RGB, three bytes a pixel, rows top to bottom.
/// </summary>
public sealed record Picture(int Width, int Height, byte[] Rgb)
{
    public (byte R, byte G, byte B) At(int x, int y)
    {
        int i = (y * Width + x) * 3;
        return (Rgb[i], Rgb[i + 1], Rgb[i + 2]);
    }
}

/// <summary>A JPEG this reader does not take, named.</summary>
public sealed class JpegNotSupportedException(string message) : Exception(message);

/// <summary>
/// Baseline JPEG (ITU-T T.81, sequential Huffman, 8-bit), with greyscale or
/// three components at any sampling factors and restart intervals. This is
/// what cameras and photo sites hand out; progressive, arithmetic-coded,
/// lossless and 12-bit files are refused by name, never half-decoded.
///
/// The OS has no JPEG decoder and gets none: pictures are converted here, at
/// build time, into QOI (<see cref="Qoi"/>), which a few dozen lines of X
/// read. Chroma is upsampled by repetition, which is invisible once the
/// picture is scaled down to a screen, the only thing this is used for.
/// </summary>
public static class Jpeg
{
    private static readonly int[] ZigZag =
    {
         0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    };

    private sealed class Huffman
    {
        public readonly int[] MinCode = new int[17];
        public readonly int[] MaxCode = new int[18];
        public readonly int[] ValPtr = new int[17];
        public byte[] Values = Array.Empty<byte>();

        public static Huffman Build(byte[] counts, byte[] values)
        {
            var h = new Huffman { Values = values };
            int code = 0, k = 0;
            for (int len = 1; len <= 16; len++)
            {
                h.ValPtr[len] = k;
                h.MinCode[len] = code;
                code += counts[len - 1];
                k += counts[len - 1];
                h.MaxCode[len] = counts[len - 1] > 0 ? code - 1 : -1;
                if (code > (1 << len)) throw new InvalidDataException("a Huffman table with more codes than its lengths allow");
                code <<= 1;
            }
            return h;
        }
    }

    private sealed class Component
    {
        public int Id, H, V, Tq, Td, Ta;
        public int BlocksW, BlocksH;     // the plane, in blocks, padded to whole MCUs
        public byte[] Plane = Array.Empty<byte>();
        public int Pred;
    }

    private sealed class Bits(byte[] data, int pos)
    {
        public int Pos = pos;
        private uint _buf;
        private int _n;
        private bool _marker;     // a marker was met: what follows reads as zeros

        public int Bit()
        {
            if (_n == 0) Fill();
            _n--;
            return (int)((_buf >> _n) & 1);
        }

        private void Fill()
        {
            byte b = 0;
            if (!_marker && Pos < data.Length)
            {
                b = data[Pos];
                if (b == 0xFF)
                {
                    byte next = Pos + 1 < data.Length ? data[Pos + 1] : (byte)0xD9;
                    if (next == 0x00) Pos += 2;
                    else { _marker = true; b = 0; }
                }
                else Pos++;
            }
            _buf = (_buf << 8) | b;
            _n = 8;
        }

        public int Receive(int s)
        {
            int v = 0;
            for (int i = 0; i < s; i++) v = (v << 1) | Bit();
            return v;
        }

        public static int Extend(int v, int s) => s == 0 ? 0 : (v < (1 << (s - 1)) ? v - (1 << s) + 1 : v);

        public int Decode(Huffman h)
        {
            int code = 0;
            for (int len = 1; len <= 16; len++)
            {
                code = (code << 1) | Bit();
                if (code <= h.MaxCode[len]) return h.Values[h.ValPtr[len] + code - h.MinCode[len]];
            }
            throw new InvalidDataException("a Huffman code that no table holds");
        }

        /// <summary>At a restart: drop the partial byte and step over RSTn.</summary>
        public void Restart(int expect)
        {
            _n = 0; _buf = 0; _marker = false;
            while (Pos + 1 < data.Length && !(data[Pos] == 0xFF && data[Pos + 1] != 0x00 && data[Pos + 1] != 0xFF)) Pos++;
            if (Pos + 1 >= data.Length || data[Pos + 1] != 0xD0 + expect)
                throw new InvalidDataException($"restart marker RST{expect} missing");
            Pos += 2;
        }
    }

    private static readonly float[,] Cos = BuildCos();
    private static float[,] BuildCos()
    {
        var c = new float[8, 8];
        for (int x = 0; x < 8; x++)
            for (int u = 0; u < 8; u++)
                c[x, u] = (float)((u == 0 ? Math.Sqrt(0.5) : 1.0) * Math.Cos((2 * x + 1) * u * Math.PI / 16) / 2);
        return c;
    }

    private static void Idct(int[] coef, byte[] plane, int stride, int ox, int oy)
    {
        Span<float> tmp = stackalloc float[64];
        for (int v = 0; v < 8; v++)
            for (int x = 0; x < 8; x++)
            {
                float s = 0;
                for (int u = 0; u < 8; u++) s += Cos[x, u] * coef[v * 8 + u];
                tmp[v * 8 + x] = s;
            }
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
            {
                float s = 0;
                for (int v = 0; v < 8; v++) s += Cos[y, v] * tmp[v * 8 + x];
                int p = (int)MathF.Round(s + 128);
                plane[(oy + y) * stride + ox + x] = (byte)Math.Clamp(p, 0, 255);
            }
    }

    private static int U16(byte[] d, int p) => (d[p] << 8) | d[p + 1];

    public static Picture Decode(byte[] data)
    {
        if (data.Length < 4 || data[0] != 0xFF || data[1] != 0xD8) throw new InvalidDataException("not a JPEG (no SOI)");
        var quant = new int[4][];
        var dc = new Huffman?[4];
        var ac = new Huffman?[4];
        Component[]? comps = null;
        int width = 0, height = 0, restart = 0, hmax = 1, vmax = 1, adobe = -1;
        bool scanned = false;
        int p = 2;

        while (p < data.Length)
        {
            if (data[p] != 0xFF) throw new InvalidDataException($"a marker expected at byte {p}");
            while (p < data.Length && data[p] == 0xFF) p++;
            if (p >= data.Length) break;
            byte m = data[p++];
            if (m == 0xD9) break;                                   // EOI
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;    // no length
            if (p + 2 > data.Length) throw new InvalidDataException("a segment cut short");
            int len = U16(data, p);
            int end = p + len;
            if (len < 2 || end > data.Length) throw new InvalidDataException("a segment longer than the file");
            int q = p + 2;

            switch (m)
            {
                case 0xC0: case 0xC1:
                {
                    if (data[q] != 8) throw new JpegNotSupportedException($"{data[q]}-bit samples (8-bit only)");
                    height = U16(data, q + 1); width = U16(data, q + 3);
                    int n = data[q + 5];
                    if (width == 0 || height == 0) throw new JpegNotSupportedException("a height set later by DNL");
                    if (n != 1 && n != 3) throw new JpegNotSupportedException($"{n} components (1 or 3 only)");
                    comps = new Component[n];
                    for (int i = 0; i < n; i++)
                    {
                        int b = q + 6 + i * 3;
                        comps[i] = new Component { Id = data[b], H = data[b + 1] >> 4, V = data[b + 1] & 15, Tq = data[b + 2] };
                        if (comps[i].H is < 1 or > 4 || comps[i].V is < 1 or > 4 || comps[i].Tq > 3)
                            throw new InvalidDataException("a component's sampling or table out of range");
                        hmax = Math.Max(hmax, comps[i].H); vmax = Math.Max(vmax, comps[i].V);
                    }
                    int mcusX = (width + 8 * hmax - 1) / (8 * hmax), mcusY = (height + 8 * vmax - 1) / (8 * vmax);
                    foreach (var c in comps)
                    {
                        c.BlocksW = mcusX * c.H; c.BlocksH = mcusY * c.V;
                        c.Plane = new byte[c.BlocksW * 8 * c.BlocksH * 8];
                    }
                    break;
                }
                case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
                case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
                    throw new JpegNotSupportedException($"SOF{m - 0xC0} ({(m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE ? "progressive" : m >= 0xC9 ? "arithmetic" : "lossless or hierarchical")}): baseline only");
                case 0xC4:
                    while (q < end)
                    {
                        int tc = data[q] >> 4, th = data[q] & 15;
                        if (tc > 1 || th > 3 || q + 17 > end) throw new InvalidDataException("a bad Huffman table");
                        var counts = new byte[16];
                        Array.Copy(data, q + 1, counts, 0, 16);
                        int total = 0; foreach (var c in counts) total += c;
                        if (q + 17 + total > end) throw new InvalidDataException("a Huffman table longer than its segment");
                        var values = new byte[total];
                        Array.Copy(data, q + 17, values, 0, total);
                        var h = Huffman.Build(counts, values);
                        if (tc == 0) dc[th] = h; else ac[th] = h;
                        q += 17 + total;
                    }
                    break;
                case 0xDB:
                    while (q < end)
                    {
                        int pq = data[q] >> 4, tq = data[q] & 15;
                        if (pq > 1 || tq > 3 || q + 1 + 64 * (pq + 1) > end) throw new InvalidDataException("a bad quantisation table");
                        var t = new int[64];
                        for (int i = 0; i < 64; i++) t[i] = pq == 0 ? data[q + 1 + i] : U16(data, q + 1 + 2 * i);
                        quant[tq] = t;
                        q += 1 + 64 * (pq + 1);
                    }
                    break;
                case 0xDD:
                    restart = U16(data, q);
                    break;
                case 0xEE:
                    if (len >= 14 && data[q] == 'A' && data[q + 1] == 'd' && data[q + 2] == 'o' && data[q + 3] == 'b' && data[q + 4] == 'e')
                        adobe = data[q + 11];
                    break;
                case 0xDA:
                {
                    if (comps is null) throw new InvalidDataException("a scan before the frame");
                    if (scanned) throw new JpegNotSupportedException("more than one scan");
                    int ns = data[q];
                    if (ns < 1 || ns > comps.Length) throw new InvalidDataException("a scan with a bad component count");
                    var scan = new Component[ns];
                    for (int i = 0; i < ns; i++)
                    {
                        int id = data[q + 1 + i * 2], t = data[q + 2 + i * 2];
                        scan[i] = Array.Find(comps, c => c.Id == id) ?? throw new InvalidDataException("a scan naming no component");
                        scan[i].Td = t >> 4; scan[i].Ta = t & 15;
                        if (scan[i].Td > 3 || scan[i].Ta > 3 || dc[scan[i].Td] is null || ac[scan[i].Ta] is null || quant[scan[i].Tq] is null)
                            throw new InvalidDataException("a scan using a table never defined");
                    }
                    if (ns != comps.Length) throw new JpegNotSupportedException("a scan of only some components");
                    end = Scan(data, end, scan, width, height, hmax, vmax, restart, dc!, ac!, quant);
                    scanned = true;
                    p = end;
                    continue;
                }
            }
            p = end;
        }

        if (comps is null || !scanned) throw new InvalidDataException("no frame or no scan");
        return ToRgb(comps, width, height, hmax, vmax, adobe);
    }

    private static int Scan(byte[] data, int start, Component[] scan, int width, int height, int hmax, int vmax,
                            int restart, Huffman[] dc, Huffman[] ac, int[][] quant)
    {
        var bits = new Bits(data, start);
        var coef = new int[64];
        foreach (var c in scan) c.Pred = 0;
        int mcusX, mcusY;
        bool single = scan.Length == 1;
        if (single)
        {
            var c = scan[0];
            mcusX = ((width * c.H + hmax - 1) / hmax + 7) / 8;
            mcusY = ((height * c.V + vmax - 1) / vmax + 7) / 8;
        }
        else
        {
            mcusX = (width + 8 * hmax - 1) / (8 * hmax);
            mcusY = (height + 8 * vmax - 1) / (8 * vmax);
        }

        int total = mcusX * mcusY, rst = 0;
        for (int mcu = 0; mcu < total; mcu++)
        {
            if (restart > 0 && mcu > 0 && mcu % restart == 0)
            {
                bits.Restart(rst);
                rst = (rst + 1) & 7;
                foreach (var c in scan) c.Pred = 0;
            }
            int mx = mcu % mcusX, my = mcu / mcusX;
            foreach (var c in scan)
            {
                int bh = single ? 1 : c.H, bv = single ? 1 : c.V;
                for (int by = 0; by < bv; by++)
                    for (int bx = 0; bx < bh; bx++)
                    {
                        Block(bits, c, coef, dc[c.Td], ac[c.Ta], quant[c.Tq]);
                        int x = (mx * bh + bx) * 8, y = (my * bv + by) * 8;
                        Idct(coef, c.Plane, c.BlocksW * 8, x, y);
                    }
            }
        }

        // Past the entropy-coded data to the next marker.
        int pos = bits.Pos;
        while (pos + 1 < data.Length && !(data[pos] == 0xFF && data[pos + 1] != 0x00 && !(data[pos + 1] >= 0xD0 && data[pos + 1] <= 0xD7)))
            pos++;
        return pos;
    }

    private static void Block(Bits bits, Component c, int[] coef, Huffman dc, Huffman ac, int[] q)
    {
        Array.Clear(coef);
        int t = bits.Decode(dc);
        if (t > 11) throw new InvalidDataException("a DC difference too wide");
        c.Pred += Bits.Extend(bits.Receive(t), t);
        coef[0] = c.Pred * q[0];
        for (int k = 1; k < 64;)
        {
            int rs = bits.Decode(ac);
            int r = rs >> 4, s = rs & 15;
            if (s == 0)
            {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) throw new InvalidDataException("an AC run past the block");
            coef[ZigZag[k]] = Bits.Extend(bits.Receive(s), s) * q[k];
            k++;
        }
    }

    private static Picture ToRgb(Component[] comps, int width, int height, int hmax, int vmax, int adobe)
    {
        var rgb = new byte[width * height * 3];
        int Sample(Component c, int x, int y) =>
            c.Plane[(y * c.V / vmax) * c.BlocksW * 8 + x * c.H / hmax];

        bool ycc = comps.Length == 3 && adobe != 0;
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                int o = (y * width + x) * 3;
                if (comps.Length == 1)
                {
                    byte g = (byte)Sample(comps[0], x, y);
                    rgb[o] = rgb[o + 1] = rgb[o + 2] = g;
                    continue;
                }
                int a = Sample(comps[0], x, y), b = Sample(comps[1], x, y), d = Sample(comps[2], x, y);
                if (!ycc) { rgb[o] = (byte)a; rgb[o + 1] = (byte)b; rgb[o + 2] = (byte)d; continue; }
                float cb = b - 128f, cr = d - 128f;
                rgb[o]     = Clamp(a + 1.402f * cr);
                rgb[o + 1] = Clamp(a - 0.344136f * cb - 0.714136f * cr);
                rgb[o + 2] = Clamp(a + 1.772f * cb);
            }
        return new Picture(width, height, rgb);
    }

    private static byte Clamp(float v) => (byte)Math.Clamp((int)MathF.Round(v), 0, 255);

    // ---- metadata ----

    /// <summary>One segment before the scan: its marker, offset and whole length (marker included).</summary>
    public readonly record struct Segment(byte Marker, int Offset, int Length)
    {
        public string Name => Marker switch
        {
            >= 0xE0 and <= 0xEF => $"APP{Marker - 0xE0}",
            0xFE => "COM",
            _ => $"0xFF{Marker:X2}",
        };
    }

    /// <summary>
    /// What a stripped file keeps: APP0 (JFIF), APP2 (the ICC colour profile,
    /// which changes how the pixels look and says nothing about anyone) and
    /// APP14 (Adobe: which colour transform the components use). EXIF and XMP
    /// (APP1, where the camera, the time and the GPS position live), IPTC
    /// (APP13), comments and every other APPn go.
    /// </summary>
    public static bool IsMetadata(byte marker) =>
        marker == 0xFE || (marker >= 0xE0 && marker <= 0xEF && marker != 0xE0 && marker != 0xE2 && marker != 0xEE);

    /// <summary>The segments from SOI up to the first scan.</summary>
    public static List<Segment> Segments(byte[] data)
    {
        if (data.Length < 4 || data[0] != 0xFF || data[1] != 0xD8) throw new InvalidDataException("not a JPEG (no SOI)");
        var list = new List<Segment>();
        int p = 2;
        while (p + 4 <= data.Length)
        {
            if (data[p] != 0xFF) throw new InvalidDataException($"a marker expected at byte {p}");
            int at = p;
            while (p < data.Length && data[p] == 0xFF) p++;
            byte m = data[p++];
            int len = U16(data, p);
            if (len < 2 || p + len > data.Length) throw new InvalidDataException("a segment longer than the file");
            list.Add(new Segment(m, at, p + len - at));
            if (m == 0xDA) break;
            p += len;
        }
        return list;
    }

    /// <summary>
    /// The file without its metadata, every other byte as it was: the
    /// entropy-coded data is copied, not re-encoded, so the pixels are exactly
    /// the original's. Returns the names of the segments dropped.
    /// </summary>
    public static byte[] Strip(byte[] data, out List<string> dropped)
    {
        var segs = Segments(data);
        if (segs.Count == 0 || segs[^1].Marker != 0xDA) throw new InvalidDataException("no scan");
        var outp = new MemoryStream(data.Length);
        outp.Write(data, 0, 2);
        dropped = new List<string>();
        foreach (var s in segs)
        {
            if (s.Marker == 0xDA) { outp.Write(data, s.Offset, data.Length - s.Offset); break; }
            if (IsMetadata(s.Marker)) { dropped.Add(s.Name); continue; }
            outp.Write(data, s.Offset, s.Length);
        }
        return outp.ToArray();
    }
}
