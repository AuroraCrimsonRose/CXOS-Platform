namespace CXEX.Core.Crypto;

public interface ICXKeyPair
{
    ICXPublicKey PublicKey { get; }
    ICXPrivateKey PrivateKey { get; }
}