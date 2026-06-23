namespace CXEX.Core.Crypto;

public interface ICXPrivateKey
{
    CXKeyType Algorithm { get; }

    // Signs the provided data hash/bytes
    byte[] Sign(byte[] data);
}