using System.Collections.Generic;
using CXEX.Core.Crypto.Signing;

namespace CXEX.Core.Crypto;

public class CXKeyStore
{
    private readonly Dictionary<string, ICXPublicKey> _trustedKeys = new();

    public void RegisterKey(ICXPublicKey key)
    {
        string fingerprint = Convert.ToBase64String(key.GetFingerprint());
        _trustedKeys[fingerprint] = key;
    }

    public ICXPublicKey? GetKey(CXFingerprint fingerprint)
    {
        string key = Convert.ToBase64String(fingerprint.Bytes);
        return _trustedKeys.GetValueOrDefault(key);
    }
}