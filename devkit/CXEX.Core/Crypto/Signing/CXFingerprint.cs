using System;
using System.Collections;
using System.Linq;

namespace CXEX.Core.Crypto.Signing;

public readonly struct CXFingerprint : IEquatable<CXFingerprint>
{
    public readonly byte[] Bytes; // 32 bytes

    public CXFingerprint(byte[] bytes)
    {
        if (bytes.Length != 32) throw new ArgumentException("Fingerprint must be 32 bytes.");
        Bytes = bytes;
    }

    public bool Equals(CXFingerprint other) => Bytes.SequenceEqual(other.Bytes);
    public override bool Equals(object? obj) => obj is CXFingerprint other && Equals(other);
    public override int GetHashCode() => StructuralComparisons.StructuralEqualityComparer.GetHashCode(Bytes);
}