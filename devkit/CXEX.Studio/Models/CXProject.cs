using System.IO;
using System.Text.Json.Serialization;

namespace CXEX.Studio.Models.Project;

public enum CxProjectType { Kernel, OS, Application, Library, Disk }

/// <summary>
/// A CX DevKit project (.cxproj, JSON). Captures everything the IDE needs to know
/// about a project's environment: target, toolchain paths, build + disk + emulator
/// settings. Stored next to the project's source folder.
/// </summary>
public sealed class CxProject
{
    /// <summary>Schema version, so older project files can be migrated later.</summary>
    public int FormatVersion { get; set; } = 1;

    public string Name { get; set; } = "Untitled";
    public string Version { get; set; } = "0.1.0";
    public CxProjectType Type { get; set; } = CxProjectType.Kernel;
    public string TargetArch { get; set; } = "i686";
    public string? Description { get; set; }

    public CxToolchain Toolchain { get; set; } = new();
    public CxBuildSettings Build { get; set; } = new();
    public CxDiskSettings Disk { get; set; } = new();
    public CxEmulatorSettings Emulator { get; set; } = new();

    /// <summary>Absolute path to the .cxproj file. Not serialized (it IS the file).</summary>
    [JsonIgnore] public string? ProjectPath { get; set; }
    [JsonIgnore] public string? ProjectDir => ProjectPath is null ? null : Path.GetDirectoryName(ProjectPath);
}

/// <summary>External tool locations. Empty = "find on PATH".</summary>
public sealed class CxToolchain
{
    public string Gcc { get; set; } = "i686-elf-gcc";
    public string Ld { get; set; } = "i686-elf-ld";
    public string Nasm { get; set; } = "nasm";
    public string CMake { get; set; } = "cmake";
    public string Qemu { get; set; } = "qemu-system-i386";
    public string Bochs { get; set; } = "bochs";
}

public sealed class CxBuildSettings
{
    public string SourceDir { get; set; } = "src";
    public string OutputDir { get; set; } = "build";
    public string EntryPoint { get; set; } = "_start";
    /// <summary>Link base address (hex, e.g. 0x100000 kernel phys, 0x400000 app).</summary>
    public string LinkBase { get; set; } = "0x100000";
    public System.Collections.Generic.List<string> ExtraFlags { get; set; } = new();
}

public sealed class CxDiskSettings
{
    public string? ImagePath { get; set; }          // path to the .img for this project
    public int DiskSizeMb { get; set; } = 64;
    public int BootSizeMb { get; set; } = 8;
    /// <summary>Sector base of the CXFS SYSTEM partition, if known (for quick mount).</summary>
    public long? CxfsBaseLba { get; set; }
}

public sealed class CxEmulatorSettings
{
    public string Engine { get; set; } = "qemu";    // "qemu" | "bochs"
    public int MemoryMb { get; set; } = 128;
    public bool SerialToConsole { get; set; } = true;
    public System.Collections.Generic.List<string> ExtraArgs { get; set; } = new();
}