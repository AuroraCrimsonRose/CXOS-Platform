using System;
using System.Collections.Generic;
using System.Linq;
using Dock.Avalonia.Controls;
using Dock.Model.Controls;
using Dock.Model.Core;
using Dock.Model.Mvvm;
using Dock.Model.Mvvm.Controls;
using CXEX.Studio.ViewModels;

namespace CXEX.Studio.Docking;

/// <summary>
/// Builds the Studio's IDE layout and owns window opening. Tools (ProjectExplorer,
/// Image) live in side ToolDocks; everything else (Dashboard, Emulator, Hex
/// inspector, editors) is a Document opened into the center DocumentDock on demand.
/// OpenDocument focuses an already-open window instead of duplicating it, so the
/// activity bar / View menu can open any window type cleanly.
/// </summary>
public class StudioDockFactory : Factory
{
    private DocumentDock? _documents;
    private ToolDock? _leftDock;
    private FileTypeInspectorViewModel? _hexInspector;
    private ProjectExplorerViewModel? _projectExplorer;
    private readonly Dictionary<string, IDockable> _open = new();

    public override IRootDock CreateLayout()
    {
        // shared inspectors / tools
        _hexInspector = new FileTypeInspectorViewModel { Id = "Hex", Title = "Hex Inspector" };
        var imageExplorer = new ImageViewModel(_hexInspector) { Id = "ImageExplorer", Title = "Image Explorer" };
        _projectExplorer = new ProjectExplorerViewModel(_hexInspector, imageExplorer)
        { Id = "ProjectExplorer", Title = "Project Explorer" };

        // center: documents open here; seed with a Dashboard so it is never empty
        var dashboard = new DashboardViewModel { Id = "Dashboard", Title = "Dashboard" };
        _open["Dashboard"] = dashboard;

        _documents = new DocumentDock
        {
            Id = "Documents",
            Title = "Documents",
            IsCollapsable = false,
            Proportion = double.NaN,
            ActiveDockable = dashboard,
            VisibleDockables = CreateList<IDockable>(dashboard)
        };

        // left: file-navigation tools
        _leftDock = new ToolDock
        {
            Id = "LeftPane",
            Title = "Explorer",
            Proportion = 0.22,
            Alignment = Alignment.Left,
            ActiveDockable = _projectExplorer,
            VisibleDockables = CreateList<IDockable>(_projectExplorer, imageExplorer)
        };

        var mainLayout = new ProportionalDock
        {
            Id = "MainLayout",
            Orientation = Orientation.Horizontal,
            Proportion = double.NaN,
            VisibleDockables = CreateList<IDockable>(
                _leftDock,
                new ProportionalDockSplitter(),
                _documents)
        };

        var rootDock = CreateRootDock();
        rootDock.IsCollapsable = false;
        rootDock.DefaultDockable = mainLayout;
        rootDock.ActiveDockable = mainLayout;
        rootDock.VisibleDockables = CreateList<IDockable>(mainLayout);
        return rootDock;
    }

    public override void InitLayout(IDockable layout)
    {
        HostWindowLocator = new Dictionary<string, Func<IHostWindow>>
        {
            [nameof(IDockWindow)] = () => new HostWindow()
        };
        base.InitLayout(layout);
    }

    // ---- window opening ----

    /// <summary>Open (or focus, if already open) a document in the center dock.</summary>
    public void OpenDocument(IDockable doc)
    {
        if (_documents is null) return;
        var existing = _documents.VisibleDockables?.FirstOrDefault(d => d.Id == doc.Id);
        if (existing is not null) { SetActiveDockable(existing); return; }
        AddDockable(_documents, doc);
        SetActiveDockable(doc);
        SetFocusedDockable(_documents, doc);
    }

    private T GetOrCreate<T>(string id, Func<T> make) where T : IDockable
    {
        if (_open.TryGetValue(id, out var d) && d is T existing) return existing;
        var created = make();
        _open[id] = created;
        return created;
    }

    public void OpenDashboard() => OpenDocument(GetOrCreate("Dashboard",
        () => new DashboardViewModel { Id = "Dashboard", Title = "Dashboard" }));

    public void OpenEmulator() => OpenDocument(GetOrCreate("Emulator",
        () => new EmulatorViewModel { Id = "Emulator", Title = "Emulator" }));

    public void OpenHexInspector()
    {
        if (_hexInspector is not null) OpenDocument(_hexInspector);
    }

    /// <summary>Open a fresh text editor document for a file path.</summary>
    public void OpenTextEditor(string path)
    {
        var id = "edit:" + path;
        OpenDocument(GetOrCreate(id, () => new TextEditorViewModel
        { Id = id, Title = System.IO.Path.GetFileName(path) }));
    }

    /// <summary>Focus the project explorer tool (activity-bar Explorer button).</summary>
    public void FocusExplorer()
    {
        if (_projectExplorer is not null) SetActiveDockable(_projectExplorer);
    }
}