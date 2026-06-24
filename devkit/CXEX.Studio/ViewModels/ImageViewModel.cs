using System.Collections.ObjectModel;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CommunityToolkit.Mvvm.Messaging;
using Dock.Model.Mvvm.Controls;
using CXEX.Studio.Messages;

namespace CXEX.Studio.ViewModels;

public partial class ImageNode : ObservableObject
{
    [ObservableProperty] private string _name = string.Empty;
    [ObservableProperty] private string _icon = "FileOutline";
    [ObservableProperty] private string _filePath = string.Empty;
    public ObservableCollection<ImageNode> Children { get; } = new();
}

public partial class ImageViewModel : Tool
{
    [ObservableProperty] private ObservableCollection<ImageNode> _diskNodes = new();
    private readonly FileTypeInspectorViewModel _hexInspector;

    public ImageViewModel(FileTypeInspectorViewModel hexInspector)
    {
        Id = "ImageExplorer";
        Title = "Disk Navigator";
        _hexInspector = hexInspector;
    }

    // NEW: Made async to prevent UI freezing, and triggers the Messenger!
    public async Task LoadDiskImageAsync(string imgPath)
    {
        // 1. Turn on the global loading spinner
        WeakReferenceMessenger.Default.Send(new GlobalBusyMessage(true, "Parsing CXFS Manifest..."));

        DiskNodes.Clear();

        // 2. Simulate the heavy lifting of reading bytes from the .img or parsing JSON
        await Task.Delay(1500);

        var rootDisk = new ImageNode { Name = System.IO.Path.GetFileName(imgPath), Icon = "Harddisk" };

        var bootPart = new ImageNode { Name = "BOOT (Partition 0)", Icon = "FolderOutline" };
        bootPart.Children.Add(new ImageNode { Name = "kernel.xkex", Icon = "ApplicationCog", FilePath = "dist/CXK_x86_32/packages/kernel.xkex" });

        var stagePart = new ImageNode { Name = "STAGE (Partition 1)", Icon = "FolderOutline" };
        stagePart.Children.Add(new ImageNode { Name = "Boot.xoex", Icon = "FileCodeOutline", FilePath = "dist/CXK_x86_32/packages/executive.xoex" });

        var systemPart = new ImageNode { Name = "SYSTEM (CXFS)", Icon = "ServerOutline" };

        // --- DYNAMIC MANIFEST DISCOVERY MOCK ---
        // Here you would deserialize your actual manifest. For now, we simulate finding the log.
        bool manifestHasCrashLog = true;

        if (manifestHasCrashLog)
        {
            systemPart.Children.Add(new ImageNode { Name = "crash.log", Icon = "AlertOctagonOutline", FilePath = "dist/CXK_x86_32/images/crash.log" });
        }
        // ---------------------------------------

        rootDisk.Children.Add(bootPart);
        rootDisk.Children.Add(stagePart);
        rootDisk.Children.Add(systemPart);

        DiskNodes.Add(rootDisk);

        // 3. Turn off the global loading spinner!
        WeakReferenceMessenger.Default.Send(new GlobalBusyMessage(false));
    }

    [RelayCommand]
    private void OpenNodeInHex(ImageNode node)
    {
        if (!string.IsNullOrEmpty(node.FilePath)) _hexInspector.LoadFile(node.FilePath);
    }
}