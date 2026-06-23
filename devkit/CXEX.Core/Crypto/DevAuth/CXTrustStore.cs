using System.Collections.Generic;

namespace CXEX.Core.Crypto.DevAuth;

public class CXTrustStore
{
    private readonly Dictionary<uint, CXDevCert> _rootCerts = new();

    public void RegisterRoot(CXDevCert rootCert)
    {
        _rootCerts[rootCert.Header.SubjectID] = rootCert;
    }

    public CXDevCert? GetRoot(uint issuerId)
    {
        return _rootCerts.TryGetValue(issuerId, out var cert) ? cert : null;
    }
}