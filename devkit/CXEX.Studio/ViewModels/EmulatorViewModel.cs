using System;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Dock.Model.Mvvm.Controls;
using CXEX.CLI.Wrappers;

namespace CXEX.Studio.ViewModels;

public partial class EmulatorViewModel : Document
{
    // These properties automatically generate INotifyPropertyChanged events
    [ObservableProperty] private int _memoryMb = 2048;
    [ObservableProperty] private int _emulatorType = 0; // 0 = QEMU, 1 = Bochs
    [ObservableProperty] private string _machineType = "q35";
    [ObservableProperty] private bool _enableAudio = true;
    [ObservableProperty] private bool _enableNetworking = true;

    // Hardcoded to your dist folder for now, but eventually this will pull from your ProjectSettings
    private string GetDiskPath()
    {
        // Adjust this if your Studio runs from a different working directory
        string path = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, "..", "dist", "CXK_x86_32", "images", "cxk_disk.img"));
        return path;
    }

    [RelayCommand]
    private void LaunchEmulator()
    {
        string diskPath = GetDiskPath();

        if (EmulatorType == 0) // QEMU
        {
            var config = new QemuConfig
            {
                MemoryMb = MemoryMb,
                MachineType = MachineType,
                BootDisk = diskPath,
                EnableAudio = EnableAudio,
                EnableNetworking = EnableNetworking
            };

            // Runs the CLI wrapper natively!
            QemuTool.Run(config);
        }
        else // Bochs
        {
            var config = new BochsConfig
            {
                MemoryMb = MemoryMb,
                BootDisk = diskPath
            };

            string imgDir = Path.GetDirectoryName(diskPath) ?? Environment.CurrentDirectory;
            BochsTool.Run(config, imgDir);
        }
    }
}