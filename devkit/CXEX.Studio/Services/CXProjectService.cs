using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;
using CXEX.Studio.Models.Project;

namespace CXEX.Studio.Services;

/// <summary>Create / load / save .cxproj files (JSON).</summary>
public static class CxProjectService
{
    public const string Extension = ".cxproj";

    private static readonly JsonSerializerOptions Opts = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        Converters = { new JsonStringEnumConverter() },
    };

    /// <summary>Create a new project: makes the folder, writes &lt;name&gt;.cxproj, returns it.</summary>
    public static CxProject Create(string name, string folder, CxProjectType type = CxProjectType.Kernel)
    {
        Directory.CreateDirectory(folder);
        var proj = new CxProject { Name = name, Type = type };
        proj.ProjectPath = Path.Combine(folder, name + Extension);
        // scaffold the conventional source/output folders
        try { Directory.CreateDirectory(Path.Combine(folder, proj.Build.SourceDir)); } catch { }
        Save(proj);
        return proj;
    }

    /// <summary>Load a .cxproj from disk.</summary>
    public static CxProject Load(string path)
    {
        var proj = JsonSerializer.Deserialize<CxProject>(File.ReadAllText(path), Opts) ?? new CxProject();
        proj.ProjectPath = Path.GetFullPath(path);
        return proj;
    }

    /// <summary>Write the project back to its ProjectPath.</summary>
    public static void Save(CxProject proj)
    {
        if (string.IsNullOrEmpty(proj.ProjectPath)) return;
        File.WriteAllText(proj.ProjectPath, JsonSerializer.Serialize(proj, Opts));
    }

    /// <summary>Find a single .cxproj in a folder (for "open folder" convenience). Null if none/many.</summary>
    public static string? FindInFolder(string folder)
    {
        if (!Directory.Exists(folder)) return null;
        var hits = Directory.GetFiles(folder, "*" + Extension);
        return hits.Length == 1 ? hits[0] : null;
    }
}