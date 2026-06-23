namespace CXEX.Core.Crypto.Signing;

public class CXSignature
{
    public ushort SigAlgo { get; set; }
    public ushort HashAlgo { get; set; }
    public CXFingerprint Fingerprint { get; set; }
    public byte[] SignatureBytes { get; set; }

    // Used to read from a file's footer
    public static CXSignature FromBytes(byte[] data)
    {
        // Implementation: Parse the 42-byte header + signature body 
        // to populate the fields above.
        return new CXSignature();
    }
}