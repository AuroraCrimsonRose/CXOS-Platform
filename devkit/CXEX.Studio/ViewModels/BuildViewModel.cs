using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CXEX.Studio.Services;

namespace CXEX.Studio.ViewModels;

/// <summary>
/// Build panel: drives BuildService (in-process CXEX.Build packaging + streaming
/// tool runner) with a live log and progress. The first wired operation packages
/// an .elf into a .xcex; extend BuildAsync with compile/link/sign/image steps using
/// the same BuildService methods to fully replace the batch/ps1 pipeline.
/// </summary>
public partial class BuildViewModel : ObservableObject
{
    private readonly BuildService _build = new();
    private CancellationTokenSource? _cts;

    [ObservableProperty] private string projectPath = string.Empty;
    [ObservableProperty] private string outputType = "user";        // kernel|boot|os|user
    [ObservableProperty] private int buildProgress;
    [ObservableProperty] private string buildStatus = "Ready";
    [ObservableProperty] private bool isBusy;

    public ObservableCollection<string> Log { get; } = new();

    private void Append(string line) =>
        Avalonia.Threading.Dispatcher.UIThread.Post(() => Log.Add(line));

    [RelayCommand(CanExecute = nameof(CanBuild))]
    private async Task BuildAsync()
    {
        IsBusy = true; BuildProgress = 0; BuildStatus = "Building...";
        Log.Clear();
        _cts = new CancellationTokenSource();
        try
        {
            if (string.IsNullOrWhiteSpace(ProjectPath) || !File.Exists(ProjectPath))
            {
                BuildStatus = "No input file"; Append("error: set a valid input path"); return;
            }

            await Task.Run(() =>
            {
                // First wired step: ELF -> CXEX via CXEX.Build (in-process).
                var outPath = Path.ChangeExtension(ProjectPath, OutputType switch
                {
                    "kernel" => ".xkex",
                    "boot" => ".xoex",
                    "os" => ".xoex",
                    _ => ".xcex"
                });
                _build.PackageCxex(ProjectPath, outPath, OutputType, Append);
                Append($"OK: {Path.GetFileName(outPath)}");
            }, _cts.Token);

            BuildProgress = 100; BuildStatus = "Build Complete";
        }
        catch (OperationCanceledException) { BuildStatus = "Canceled"; Append("canceled"); }
        catch (Exception ex) { BuildStatus = "Build Failed"; Append($"error: {ex.Message}"); }
        finally { IsBusy = false; _cts?.Dispose(); _cts = null; BuildCommand.NotifyCanExecuteChanged(); }
    }

    private bool CanBuild() => !IsBusy;

    [RelayCommand]
    private void Cancel() => _cts?.Cancel();

    partial void OnIsBusyChanged(bool value) => BuildCommand.NotifyCanExecuteChanged();
}