using System;
using System.IO;
using System.Text;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Dock.Model.Mvvm.Controls;

namespace CXEX.Studio.ViewModels;

/// <summary>
/// Hex viewer. Reads only the current 4KB page from disk (works on multi-GB disk
/// images), detects the file's magic for a metadata panel + highlight, and supports
/// address / hex / ascii search that scans the file in chunks and jumps to matches.
/// </summary>
public partial class FileTypeInspectorViewModel : Document
{
    public const int PageSize = 4096;          // 4 KB page
    private const int K4 = 4096, M2 = 2 * 1024 * 1024, M4 = 4 * 1024 * 1024;

    private string? _path;

    [ObservableProperty] private string _fileName = "No File Loaded";
    [ObservableProperty] private string _fileSize = "0 bytes";
    [ObservableProperty] private long _fileLength;
    [ObservableProperty] private long _pageOffset;        // start of current page
    [ObservableProperty] private byte[]? _pageData;       // current page bytes (bound to HexView.Data)
    [ObservableProperty] private string _pageLabel = "page 0 of 0";

    // magic / format
    [ObservableProperty] private string _formatName = "Unknown";
    [ObservableProperty] private string _magicHex = "-";
    [ObservableProperty] private long _highlightStart = -1;   // global magic offset
    [ObservableProperty] private int _highlightLength;

    // search
    public string[] SearchModes { get; } = { "Hex", "ASCII", "Address" };
    [ObservableProperty] private string _searchMode = "Hex"; // Address | Hex | ASCII
    [ObservableProperty] private string _searchText = "";
    [ObservableProperty] private string _searchStatus = "";
    [ObservableProperty] private long _matchStart = -1;
    [ObservableProperty] private int _matchLength;

    public void LoadFile(string path)
    {
        if (!File.Exists(path)) return;
        _path = path;
        FileName = Path.GetFileName(path);
        FileLength = new FileInfo(path).Length;
        FileSize = FileLength < 1024 ? $"{FileLength} bytes" :
                   FileLength < M2 ? $"{FileLength / 1024.0:F1} KB" : $"{FileLength / 1048576.0:F2} MB";
        Title = $"Hex: {FileName}";
        DetectMagic();
        GoToOffset(0);
    }

    // ---- paging ----
    private void ReadPage()
    {
        if (_path is null) { PageData = Array.Empty<byte>(); return; }
        try
        {
            using var fs = new FileStream(_path, FileMode.Open, FileAccess.Read, FileShare.Read);
            fs.Seek(PageOffset, SeekOrigin.Begin);
            int len = (int)Math.Min(PageSize, FileLength - PageOffset);
            var buf = new byte[Math.Max(0, len)];
            int read = 0; while (read < buf.Length) { int n = fs.Read(buf, read, buf.Length - read); if (n == 0) break; read += n; }
            PageData = buf;
        }
        catch { PageData = Array.Empty<byte>(); }

        long page = PageOffset / PageSize + 1;
        long total = Math.Max(1, (FileLength + PageSize - 1) / PageSize);
        PageLabel = $"page {page} of {total}  (0x{PageOffset:X})";
    }

    private void GoToOffset(long offset)
    {
        long max = Math.Max(0, ((FileLength - 1) / PageSize) * PageSize);
        PageOffset = Math.Clamp(offset / PageSize * PageSize, 0, max);
        ReadPage();
    }

    [RelayCommand] private void Back4K() => GoToOffset(PageOffset - K4);
    [RelayCommand] private void Back2M() => GoToOffset(PageOffset - M2);
    [RelayCommand] private void Back4M() => GoToOffset(PageOffset - M4);
    [RelayCommand] private void Fwd4K() => GoToOffset(PageOffset + K4);
    [RelayCommand] private void Fwd2M() => GoToOffset(PageOffset + M2);
    [RelayCommand] private void Fwd4M() => GoToOffset(PageOffset + M4);

    // ---- magic detection (local table; can defer to CXEX.FileType later) ----
    private void DetectMagic()
    {
        HighlightStart = -1; HighlightLength = 0; FormatName = "Unknown"; MagicHex = "-";
        if (_path is null || FileLength < 4) return;
        int headLen = (int)Math.Min(1024, FileLength);   // enough to reach LBA1 (512) + boot sig (510)
        byte[] head = new byte[headLen];
        try { using var fs = File.OpenRead(_path); int r = 0; while (r < headLen) { int n = fs.Read(head, r, headLen - r); if (n == 0) break; r += n; } } catch { return; }

        (string name, byte[] sig)[] table =
        {
            ("ELF object",      new byte[]{0x7F,0x45,0x4C,0x46}),
            ("CXEX executable", new byte[]{0x43,0x58,0x45,0x58}), // "CXEX"
            ("CXFS volume",     new byte[]{0x43,0x58,0x46,0x53}), // "CXFS" (confirm byte order)
            ("XBPT table",      new byte[]{0x58,0x42,0x50,0x54}), // "XBPT"
            ("PNG image",       new byte[]{0x89,0x50,0x4E,0x47}),
            ("MZ / PE",         new byte[]{0x4D,0x5A}),
        };
        foreach (var (name, sig) in table)
        {
            if (head.Length >= sig.Length && StartsWith(head, sig))
            {
                FormatName = name;
                HighlightStart = 0; HighlightLength = sig.Length;
                var sb = new StringBuilder();
                foreach (var b in sig) sb.Append(b.ToString("X2")).Append(' ');
                MagicHex = sb.ToString().Trim();
                return;
            }
        }

        // disk-image fallbacks: XBPT at LBA1, or an MBR boot signature
        if (head.Length >= 516 && head[512] == 0x58 && head[513] == 0x42 && head[514] == 0x50 && head[515] == 0x54)
        {
            FormatName = "XBPT disk image"; HighlightStart = 512; HighlightLength = 4; MagicHex = "58 42 50 54 @ LBA1"; return;
        }
        if (head.Length >= 512 && head[510] == 0x55 && head[511] == 0xAA)
        {
            FormatName = "MBR / boot sector"; HighlightStart = 510; HighlightLength = 2; MagicHex = "55 AA @ 0x1FE"; return;
        }
    }

    private static bool StartsWith(byte[] data, byte[] sig)
    {
        for (int i = 0; i < sig.Length; i++) if (data[i] != sig[i]) return false;
        return true;
    }

    // ---- search ----
    [RelayCommand]
    private void Find()
    {
        if (_path is null || string.IsNullOrWhiteSpace(SearchText)) return;
        try
        {
            if (SearchMode == "Address")
            {
                long addr = ParseOffset(SearchText);
                if (addr < 0 || addr >= FileLength) { SearchStatus = "out of range"; return; }
                MatchStart = addr; MatchLength = 1; GoToOffset(addr);
                SearchStatus = $"at 0x{addr:X}";
                return;
            }

            byte[] pattern = SearchMode == "Hex" ? ParseHex(SearchText) : Encoding.UTF8.GetBytes(SearchText);
            if (pattern.Length == 0) { SearchStatus = "empty pattern"; return; }

            long from = (MatchStart >= 0 ? MatchStart + 1 : 0);
            long found = ScanForward(pattern, from);
            if (found < 0 && from > 0) found = ScanForward(pattern, 0); // wrap
            if (found < 0) { SearchStatus = "not found"; MatchStart = -1; MatchLength = 0; return; }

            MatchStart = found; MatchLength = pattern.Length;
            GoToOffset(found);
            SearchStatus = $"found at 0x{found:X}";
        }
        catch (Exception ex) { SearchStatus = ex.Message; }
    }

    private long ScanForward(byte[] pattern, long from)
    {
        const int chunk = 1 << 16;
        var buf = new byte[chunk + pattern.Length];
        using var fs = new FileStream(_path!, FileMode.Open, FileAccess.Read, FileShare.Read);
        long pos = from;
        while (pos < FileLength)
        {
            fs.Seek(pos, SeekOrigin.Begin);
            int want = (int)Math.Min(buf.Length, FileLength - pos);
            int read = 0; while (read < want) { int n = fs.Read(buf, read, want - read); if (n == 0) break; read += n; }
            for (int i = 0; i + pattern.Length <= read; i++)
            {
                bool ok = true;
                for (int j = 0; j < pattern.Length; j++) if (buf[i + j] != pattern[j]) { ok = false; break; }
                if (ok) return pos + i;
            }
            if (read < want) break;
            pos += chunk; // overlap of pattern.Length retained by buffer sizing
        }
        return -1;
    }

    private static long ParseOffset(string s)
    {
        s = s.Trim();
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            return Convert.ToInt64(s[2..], 16);
        return long.TryParse(s, out var d) ? d : Convert.ToInt64(s, 16);
    }

    private static byte[] ParseHex(string s)
    {
        var clean = s.Replace(" ", "").Replace("0x", "").Replace(",", "");
        if (clean.Length % 2 != 0) clean = "0" + clean;
        var bytes = new byte[clean.Length / 2];
        for (int i = 0; i < bytes.Length; i++) bytes[i] = Convert.ToByte(clean.Substring(i * 2, 2), 16);
        return bytes;
    }
}