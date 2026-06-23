using System.Runtime.InteropServices;
using CXEX.Core.FileTypes.Interfaces;

namespace CXEX.Core.FileTypes.Types;

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct CXPKHeader { /* ... as defined in spec ... */ }

public class XKPKFile : ICXFile
{
    public CXPKHeader Header;
    public byte[] Modulus;

    public void Load(byte[] data) { /* Parse header then modulus */ }
}