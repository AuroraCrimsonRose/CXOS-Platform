using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using CXEX.FileType.Types;

namespace CXEX.Crypto.Trust;

public class CXKeyStore
{
    // Indexes trusted keys by their Hex-encoded SHA-256 fingerprint
    private readonly Dictionary<string, XKPKFile> _trustedKeys = new(StringComparer.OrdinalIgnoreCase);

    public void ImportKey(string xkpkFilePath)
    {
        byte[] rawBytes = File.ReadAllBytes(xkpkFilePath);
        var pk = new XKPKFile();
        pk.Load(rawBytes);

        // Your signcxex.py computes the fingerprint over the entire raw .xkpk file
        string fingerprint = Convert.ToHexString(SHA256.HashData(rawBytes));
        _trustedKeys[fingerprint] = pk;
    }

    public XKPKFile? GetKeyByFingerprint(byte[] fingerprintBytes)
    {
        string hex = Convert.ToHexString(fingerprintBytes);
        return _trustedKeys.TryGetValue(hex, out var key) ? key : null;
    }

    public bool IsTrusted(byte[] fingerprintBytes) => GetKeyByFingerprint(fingerprintBytes) != null;
}