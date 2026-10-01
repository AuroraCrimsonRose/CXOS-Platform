using System;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace CXEX.Studio.Controls;

/// <summary>
/// Fast hex dump control. Draws the current page directly with FormattedText
/// (no per-byte visual tree), so a 4KB page is ~3 text draws per row instead of
/// thousands of Borders. Highlights magic bytes (pink) and a search match (cyan)
/// by coloring spans within each row. The ViewModel feeds it one page at a time.
/// </summary>
public class HexView : Control
{
    private const int BytesPerRow = 16;

    public static readonly StyledProperty<byte[]?> DataProperty =
        AvaloniaProperty.Register<HexView, byte[]?>(nameof(Data));
    public static readonly StyledProperty<long> BaseOffsetProperty =
        AvaloniaProperty.Register<HexView, long>(nameof(BaseOffset));
    public static readonly StyledProperty<long> HighlightStartProperty =
        AvaloniaProperty.Register<HexView, long>(nameof(HighlightStart), -1);
    public static readonly StyledProperty<int> HighlightLengthProperty =
        AvaloniaProperty.Register<HexView, int>(nameof(HighlightLength));
    public static readonly StyledProperty<long> MatchStartProperty =
        AvaloniaProperty.Register<HexView, long>(nameof(MatchStart), -1);
    public static readonly StyledProperty<int> MatchLengthProperty =
        AvaloniaProperty.Register<HexView, int>(nameof(MatchLength));

    public byte[]? Data { get => GetValue(DataProperty); set => SetValue(DataProperty, value); }
    public long BaseOffset { get => GetValue(BaseOffsetProperty); set => SetValue(BaseOffsetProperty, value); }
    public long HighlightStart { get => GetValue(HighlightStartProperty); set => SetValue(HighlightStartProperty, value); }
    public int HighlightLength { get => GetValue(HighlightLengthProperty); set => SetValue(HighlightLengthProperty, value); }
    public long MatchStart { get => GetValue(MatchStartProperty); set => SetValue(MatchStartProperty, value); }
    public int MatchLength { get => GetValue(MatchLengthProperty); set => SetValue(MatchLengthProperty, value); }

    private readonly Typeface _tf = new(FontFamily.Parse("Cascadia Code, Consolas, Courier New, monospace"));
    private const double FontSize = 13;
    private double _charW;
    private double _lineH;

    private readonly IBrush _dim = new SolidColorBrush(Color.Parse("#6E84A3"));
    private readonly IBrush _text = new SolidColorBrush(Color.Parse("#EAF4FF"));
    private readonly IBrush _accent = new SolidColorBrush(Color.Parse("#5BC0F8"));  // match
    private readonly IBrush _magic = new SolidColorBrush(Color.Parse("#F48FB1"));  // magic

    static HexView()
    {
        AffectsRender<HexView>(DataProperty, BaseOffsetProperty, HighlightStartProperty,
                               HighlightLengthProperty, MatchStartProperty, MatchLengthProperty);
        AffectsMeasure<HexView>(DataProperty);
    }

    private void EnsureMetrics()
    {
        if (_charW > 0) return;
        var probe = new FormattedText("0", CultureInfo.InvariantCulture, FlowDirection.LeftToRight, _tf, FontSize, _text);
        _charW = probe.WidthIncludingTrailingWhitespace;
        _lineH = probe.Height + 2;
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        EnsureMetrics();
        int rows = Data is null ? 0 : (Data.Length + BytesPerRow - 1) / BytesPerRow;
        double cols = 10 /*offset+gap*/ + BytesPerRow * 3 /*hex*/ + 2 /*gap*/ + BytesPerRow /*ascii*/;
        return new Size(cols * _charW + 16, Math.Max(rows, 1) * _lineH + 8);
    }

    public override void Render(DrawingContext ctx)
    {
        base.Render(ctx);
        EnsureMetrics();
        var data = Data;
        if (data is null || data.Length == 0)
        {
            DrawText(ctx, "No data", 8, 6, _dim);
            return;
        }

        double offX = 8;
        double hexX = offX + 10 * _charW;
        double asciiX = hexX + (BytesPerRow * 3) * _charW + 2 * _charW;
        int rows = (data.Length + BytesPerRow - 1) / BytesPerRow;

        for (int r = 0; r < rows; r++)
        {
            double y = 6 + r * _lineH;
            int rowStart = r * BytesPerRow;
            long rowGlobal = BaseOffset + rowStart;

            // offset column
            DrawText(ctx, rowGlobal.ToString("X8"), offX, y, _dim);

            // build hex + ascii strings for the row
            var hex = new System.Text.StringBuilder(BytesPerRow * 3);
            var asc = new System.Text.StringBuilder(BytesPerRow);
            int count = Math.Min(BytesPerRow, data.Length - rowStart);
            for (int c = 0; c < count; c++)
            {
                byte b = data[rowStart + c];
                hex.Append(b.ToString("X2")).Append(' ');
                asc.Append(b >= 32 && b <= 126 ? (char)b : '.');
            }

            var hexFt = new FormattedText(hex.ToString(), CultureInfo.InvariantCulture,
                                          FlowDirection.LeftToRight, _tf, FontSize, _text);
            var ascFt = new FormattedText(asc.ToString(), CultureInfo.InvariantCulture,
                                          FlowDirection.LeftToRight, _tf, FontSize, _text);

            // color magic + match spans within the row
            ColorSpan(hexFt, ascFt, rowGlobal, count, HighlightStart, HighlightLength, _magic);
            ColorSpan(hexFt, ascFt, rowGlobal, count, MatchStart, MatchLength, _accent);

            ctx.DrawText(hexFt, new Point(hexX, y));
            ctx.DrawText(ascFt, new Point(asciiX, y));
        }
    }

    // Apply a brush to the bytes of [start,start+len) that fall in this row.
    private static void ColorSpan(FormattedText hex, FormattedText asc, long rowGlobal, int count,
                                  long start, int len, IBrush brush)
    {
        if (start < 0 || len <= 0) return;
        long end = start + len;
        for (int c = 0; c < count; c++)
        {
            long g = rowGlobal + c;
            if (g >= start && g < end)
            {
                hex.SetForegroundBrush(brush, c * 3, 2);  // "AB" within "AB "
                asc.SetForegroundBrush(brush, c, 1);
            }
        }
    }

    private void DrawText(DrawingContext ctx, string s, double x, double y, IBrush brush)
    {
        var ft = new FormattedText(s, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, _tf, FontSize, brush);
        ctx.DrawText(ft, new Point(x, y));
    }
}