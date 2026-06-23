using System;
using System.IO;

namespace CXEX.Core.FileTypes;

public abstract class CXFileBase : ICXFile
{
    public abstract void Load(byte[] data);
    public abstract string GetDisplayName();

    protected void ValidateMagic(ReadOnlySpan<byte> data, ReadOnlySpan<byte> expectedMagic)
    {
        if (data.Length < expectedMagic.Length || !data.Slice(0, expectedMagic.Length).SequenceEqual(expectedMagic))
            throw new InvalidDataException("Invalid magic bytes for file type.");
    }
}