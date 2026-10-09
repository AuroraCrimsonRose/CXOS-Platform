// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿using System;
using System.IO;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Dock.Model.Mvvm.Controls;
using CXEX.Tools;

namespace CXEX.Studio.ViewModels;

public partial class EmulatorViewModel : Document
{
    // These properties automatically generate INotifyPropertyChanged events
    [ObservableProperty] private int _memoryMb = 2048;
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

        var config = new QemuConfig
        {
            MemoryMb = MemoryMb,
            MachineType = MachineType,
            BootDisk = diskPath,
            EnableAudio = EnableAudio,
            EnableNetworking = EnableNetworking
        };

        // Through CXEX.Tools, which is why Studio no longer references
        // CXEX.CLI: this used to reach into the command-line front end's
        // wrappers to launch an emulator (DevKit engineering §8).
        QemuTool.Run(config);
    }
}