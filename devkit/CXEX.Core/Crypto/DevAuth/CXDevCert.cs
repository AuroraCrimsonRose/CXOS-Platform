using System.Runtime.InteropServices;
using CXEX.Core.Crypto.XKPK;

namespace CXEX.Core.Crypto.DevAuth;

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct CXDevCertHeader
{
    public unsafe fixed byte Magic[4]; // 'CXDC'
    public uint SerialNumber;
    public uint IssuerID;
    public uint SubjectID;
    public uint ValidFrom;
    public uint ValidTo;
}

public class CXDevCert
{
    public CXDevCertHeader Header { get; init; }
    public XKPKKey PublicKey { get; init; } // Reference to the public key in this cert
    public byte[] Signature { get; init; }  // Signature of this cert by the issuer

    public bool Verify(ICXPublicKey issuerKey)
    {
        // Verify this cert's signature using the issuer's key
        return issuerKey.Verify(GetHeaderBytes(), Signature);
    }

    private byte[] GetHeaderBytes()
    {
        // Return raw bytes of header + public key for verification
        return Array.Empty<byte>(); // Placeholder for serialization logic
    }
}