using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Dock.Model.Mvvm.Controls;
using CXEX.Studio.Models;

namespace CXEX.Studio.ViewModels;

public partial class FileTypeInspectorViewModel : Document
{
    private byte[] _fileData = Array.Empty<byte>();
    private readonly List<HexAnnotation> _annotations = new();
    private const int PageSize = 4096; // 4KB per page

    [ObservableProperty] private ObservableCollection<HexRow> _hexRows = new();
    [ObservableProperty] private string _fileName = "No File Loaded";
    [ObservableProperty] private string _fileSize = "0 bytes";
    [ObservableProperty] private int _currentPage = 1;
    [ObservableProperty] private int _totalPages = 1;

    public void LoadFile(string path)
    {
        if (!File.Exists(path)) return;

        _fileData = File.ReadAllBytes(path);
        FileName = Path.GetFileName(path);
        FileSize = $"{_fileData.Length} bytes";
        TotalPages = (int)Math.Ceiling((double)_fileData.Length / PageSize);
        CurrentPage = 1;
        Title = $"Hex: {FileName}";

        ScanForSignatures();
        RenderPage();
    }

    private void ScanForSignatures()
    {
        _annotations.Clear();

        // Scan for CXEX Header (0x43, 0x58, 0x45, 0x58)
        if (_fileData.Length >= 4 && _fileData[0] == 0x43 && _fileData[1] == 0x58 && _fileData[2] == 0x45 && _fileData[3] == 0x58)
        {
            _annotations.Add(new HexAnnotation { StartOffset = 0, Length = 64, Name = "CXEX Executable Header", ColorHex = "#005a9e" });
        }

        // Scan the whole file for XBPT and KSNT markers
        for (int i = 0; i < _fileData.Length - 4; i++)
        {
            // XBPT Marker
            if (_fileData[i] == 0x58 && _fileData[i + 1] == 0x42 && _fileData[i + 2] == 0x50 && _fileData[i + 3] == 0x54)
            {
                _annotations.Add(new HexAnnotation { StartOffset = i, Length = 512, Name = "XBPT Partition Table", ColorHex = "#68217a" });
            }
            // KSNT Marker (Stage 2 Patch Point)
            else if (_fileData[i] == 0x54 && _fileData[i + 1] == 0x4E && _fileData[i + 2] == 0x53 && _fileData[i + 3] == 0x4B)
            {
                _annotations.Add(new HexAnnotation { StartOffset = i, Length = 8, Name = "KSNT Kernel Size Marker", ColorHex = "#107c10" });
            }
        }
    }

    [RelayCommand]
    private void NextPage()
    {
        if (CurrentPage < TotalPages) { CurrentPage++; RenderPage(); }
    }

    [RelayCommand]
    private void PrevPage()
    {
        if (CurrentPage > 1) { CurrentPage--; RenderPage(); }
    }

    private void RenderPage()
    {
        HexRows.Clear();
        int startOffset = (CurrentPage - 1) * PageSize;
        int endOffset = Math.Min(startOffset + PageSize, _fileData.Length);

        for (int i = startOffset; i < endOffset; i += 16)
        {
            var row = new HexRow { Offset = i.ToString("X8") };
            string ascii = "";

            for (int j = 0; j < 16; j++)
            {
                if (i + j < endOffset)
                {
                    byte b = _fileData[i + j];
                    char c = (b >= 32 && b <= 126) ? (char)b : '.';
                    ascii += c;

                    // Check if this byte falls inside an annotation
                    var activeAnnotation = _annotations.FirstOrDefault(a => (i + j) >= a.StartOffset && (i + j) < (a.StartOffset + a.Length));

                    row.Bytes.Add(new HexCell
                    {
                        Text = b.ToString("X2"),
                        Ascii = c.ToString(),
                        Background = activeAnnotation?.ColorHex ?? "Transparent",
                        Foreground = activeAnnotation != null ? "#FFFFFF" : "#D4D4D4",
                        ToolTip = activeAnnotation?.Name ?? $"Offset: 0x{(i + j):X8}"
                    });
                }
                else
                {
                    row.Bytes.Add(new HexCell { Text = "  ", Ascii = " " });
                }
            }
            row.AsciiData = ascii;
            HexRows.Add(row);
        }
    }
}