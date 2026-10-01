using System.Collections.Generic;
using System.IO;

namespace CXEX.Core.Interfaces;

public interface ICXFileSystem
{
    string Name { get; }
    void Mount(string path);
    void Unmount();
    bool Exists(string absolutePath);
    byte[] Read(string absolutePath);
    void Write(string absolutePath, byte[] data);
    IEnumerable<string> List(string absolutePath);
    Stream OpenFile(string absolutePath);
}