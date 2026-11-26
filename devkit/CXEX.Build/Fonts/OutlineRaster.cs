// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;

namespace CXEX.Build.Fonts;

/// <summary>A drawn glyph: coverage 0-255, row-major, placed relative to the pen and baseline.</summary>
public sealed record GlyphBitmap(int Left, int Top, int Width, int Height, byte[] Coverage, int Advance26_6)
{
    public byte At(int x, int y) => Coverage[y * Width + x];
}

/// <summary>
/// The rasteriser, as the OS will run it (`docs/system/CX_FONTS.md` §6.3).
/// This copy is the oracle: the X rasteriser (2.8a′3) is held to it pixel
/// for pixel, as `os/xc` is held to the C# compiler. So it is written for
/// that: integers only, every division of non-negative numbers (no rounding
/// direction to disagree on), and a fixed order of operations.
///
/// The method is the area-and-cover accumulation of FreeType's `smooth`
/// rasteriser and libart (from memory; neither tree is on disk): each line
/// is cut at every pixel row and column it crosses, and each piece adds to
/// its cell the height it spans ("cover") and that height times twice its
/// mean distance from the cell's left edge ("area"). A sweep along each row
/// turns those into exact coverage. The winding is non-zero by magnitude, as
/// FreeType's default: |sum| clamped to a full pixel. SerenityOS's
/// `EdgeFlagPathRasterizer` (sampled edge flags) was the other candidate;
/// it is exact only to its sample count, which an oracle would have to
/// reproduce bit for bit as well.
/// </summary>
public static class OutlineRaster
{
    /// <summary>The largest size drawn, in pixels per em.</summary>
    public const int MaxPpem = 1024;
    /// <summary>A curve is cut into at most this many lines.</summary>
    public const int MaxSteps = 16;

    /// <summary>
    /// An em coordinate to 26.6 pixels, rounded: floor((v * ppem + 32) / 64).
    /// The bias of 4 em keeps the shifted number non-negative; 4 em times any
    /// ppem is a whole number of 64ths, so the bias comes back off exactly.
    /// </summary>
    public static long Scale(int v, int ppem)
        => ((((long)v + XfntOutline.Limit) * ppem + 32) >> 6) - (long)XfntOutline.Limit * ppem / 64;

    public static GlyphBitmap Draw(OGlyph g, int ppem)
    {
        if (ppem is < 1 or > MaxPpem) throw new ArgumentOutOfRangeException(nameof(ppem));
        int adv = (int)Scale(g.Advance, ppem);
        if (g.Contours.Count == 0) return new GlyphBitmap(0, 0, 0, 0, Array.Empty<byte>(), adv);

        // The box, in whole pixels, y down. Every point is inside the glyph's
        // box and the scale keeps order, so every scaled point is inside this.
        long x0 = Scale(g.XMin, ppem), x1 = Scale(g.XMax, ppem);
        long yTop = -Scale(g.YMax, ppem), yBot = -Scale(g.YMin, ppem);
        // note: -Scale(y) is the y-down 26.6 coordinate
        int left = (int)(x0 >> 6), top = (int)FloorDiv64(yTop);
        int right = (int)((x1 + 63) >> 6), bottom = (int)CeilDiv64(yBot);
        int w = right - left, h = bottom - top;
        if (w == 0) w = 1;
        if (h == 0) h = 1;

        var acc = new Acc(w, h);
        long ox = (long)left * 64, oy = (long)top * 64;
        foreach (var c in g.Contours)
        {
            int n = c.Count;
            (long X, long Y) P(int i) => (Scale(c[i % n].X, ppem) - ox, -Scale(c[i % n].Y, ppem) - oy);
            int k = 0;
            while (k < n)
            {
                var a = P(k);
                if (c[(k + 1) % n].OnCurve) { acc.Line(a, P(k + 1)); k += 1; }
                else { acc.Quad(a, P(k + 1), P(k + 2)); k += 2; }
            }
        }
        return new GlyphBitmap(left, top, w, h, acc.Sweep(), adv);
    }

    // y-down values can be negative (a glyph above the baseline); these two
    // are the only places that divide one, and they are written out.
    private static long FloorDiv64(long v) => v >= 0 ? v >> 6 : -((-v + 63) >> 6);
    private static long CeilDiv64(long v) => -FloorDiv64(-v);

    /// <summary>The cells: per pixel a cover and an area; one spare column on the right.</summary>
    private sealed class Acc
    {
        private readonly int _w, _h;
        private readonly long[] _cover, _area;
        public Acc(int w, int h) { _w = w; _h = h; _cover = new long[(w + 1) * h]; _area = new long[(w + 1) * h]; }

        public void Quad((long X, long Y) p0, (long X, long Y) p1, (long X, long Y) p2)
        {
            long dx = p0.X - 2 * p1.X + p2.X, dy = p0.Y - 2 * p1.Y + p2.Y;
            long dev = Math.Max(Math.Abs(dx), Math.Abs(dy));
            // a quadratic cut into n chords strays by at most dev / (4 n^2) 64ths;
            // keep that within an eighth of a pixel (8): dev <= 32 n^2
            int n = 1;
            while (n < MaxSteps && dev > 32L * n * n) n++;
            long nn = (long)n * n;
            var prev = p0;
            for (int i = 1; i <= n; i++)
            {
                long u = n - i;
                // all three weights and every point are non-negative
                long x = (u * u * p0.X + 2 * u * i * p1.X + (long)i * i * p2.X + nn / 2) / nn;
                long y = (u * u * p0.Y + 2 * u * i * p1.Y + (long)i * i * p2.Y + nn / 2) / nn;
                var p = i == n ? p2 : (x, y);
                Line(prev, p);
                prev = p;
            }
        }

        public void Line((long X, long Y) a, (long X, long Y) b)
        {
            if (a.Y == b.Y) return;
            int sign = 1;
            if (a.Y > b.Y) { (a, b) = (b, a); sign = -1; }
            long dy = b.Y - a.Y;
            // x where the line meets height y, a.Y <= y <= b.Y, dividing non-negatives only
            long XAt(long y) => y == b.Y ? b.X
                : b.X >= a.X ? a.X + (y - a.Y) * (b.X - a.X) / dy
                             : a.X - (y - a.Y) * (a.X - b.X) / dy;

            for (long r = a.Y >> 6; r * 64 < b.Y; r++)
            {
                long ya = Math.Max(a.Y, r * 64), yb = Math.Min(b.Y, r * 64 + 64);
                if (ya == yb) continue;
                Row((int)r, XAt(ya), ya - r * 64, XAt(yb), yb - r * 64, sign);
            }
        }

        /// <summary>A piece inside one row, fy rising from fya to fyb (0-64).</summary>
        private void Row(int r, long xa, long fya, long xb, long fyb, int sign)
        {
            if (xa == xb) { Cell(r, xa, xa, fyb - fya, sign); return; }
            long dfy = fyb - fya;
            if (xa < xb)
            {
                long fy = fya, x = xa;
                while (x < xb)
                {
                    long edge = Math.Min((x >> 6) * 64 + 64, xb);
                    long ny = edge == xb ? fyb : fya + (edge - xa) * dfy / (xb - xa);
                    Cell(r, x, edge, ny - fy, sign);
                    x = edge; fy = ny;
                }
            }
            else
            {
                long fy = fya, x = xa;
                while (x > xb)
                {
                    long edge = Math.Max(((x - 1) >> 6) * 64, xb);
                    long ny = edge == xb ? fyb : fya + (xa - edge) * dfy / (xa - xb);
                    Cell(r, x, edge, ny - fy, sign);
                    x = edge; fy = ny;
                }
            }
        }

        /// <summary>A piece within one cell, from x0 to x1 (26.6, absolute), height d.</summary>
        private void Cell(int r, long x0, long x1, long d, int sign)
        {
            if (d == 0) return;
            // every x is at most 64 * width, so c is at most width: the spare column
            long c = Math.Min(x0, x1) >> 6;
            long fx = x0 + x1 - 2 * c * 64; // fx0 + fx1
            int at = r * (_w + 1) + (int)c;
            _cover[at] += sign * d;
            _area[at] += sign * d * fx;
        }

        public byte[] Sweep()
        {
            var o = new byte[_w * _h];
            for (int r = 0; r < _h; r++)
            {
                long cover = 0;
                for (int c = 0; c < _w; c++)
                {
                    int at = r * (_w + 1) + c;
                    cover += _cover[at];
                    // twice the covered area of the pixel, in 64ths squared: a full pixel is 8192
                    long v = cover * 128 - _area[at];
                    if (v < 0) v = -v;
                    v >>= 5;
                    o[r * _w + c] = (byte)(v > 255 ? 255 : v);
                }
            }
            return o;
        }
    }
}
