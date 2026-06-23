using System.Collections.Generic;
using System.Linq;

namespace CXEX.Core.Crypto.DevAuth;

public class CXAuthChain
{
    private readonly List<CXDevCert> _chain = new();

    public void AddLink(CXDevCert cert) => _chain.Add(cert);

    public bool Validate(ICXPublicKey rootKey)
    {
        ICXPublicKey currentSigner = rootKey;

        // Traverse chain and verify each link
        foreach (var cert in _chain)
        {
            if (!cert.Verify(currentSigner))
                return false;

            // The cert becomes the signer for the next link
            currentSigner = cert.PublicKey;
        }

        return true;
    }
}