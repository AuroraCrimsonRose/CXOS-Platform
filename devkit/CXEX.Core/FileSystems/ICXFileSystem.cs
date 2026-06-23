namespace CXEX.Core.FileSystems;

public interface ICXFileSystem
{
    // Resolves a path (e.g., "SYSTEM:/kernel.xkex") into a stream
    Stream OpenFile(string path);
    // Checks if a file exists
    bool Exists(string path);
}