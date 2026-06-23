using CXEX.FileType.Types;
using CXEX.Crypto.Signing;
using CXEX.Core.Constants;

namespace CXEX.Crypto.Trust;

public class CXAuthChain
{
    private readonly CXKeyStore _keyStore;

    public CXAuthChain(CXKeyStore keyStore)
    {
        _keyStore = keyStore;
    }

    public string AuthorizeLoad(CXEXExecutable exe, byte[] rawExecutableBytes)
    {
        if (!exe.Header.IsSigned || exe.Signature == null)
            return "REJECTED: Image is unsigned or lacks a CXSG block.";

        // 1. IDENTITY CHECK
        var trustedKey = _keyStore.GetKeyByFingerprint(exe.Signature.Fingerprint);
        if (trustedKey == null)
            return "REJECTED: Image signed by an untrusted or unknown key fingerprint.";

        // 2. INTEGRITY CHECK
        var verifyResult = CXVerifier.VerifyIntegrity(exe, rawExecutableBytes, trustedKey);
        if (verifyResult != CXVerifier.VerifyResult.Ok)
            return $"REJECTED: Cryptographic verification failed ({verifyResult}).";

        // 3. POLICY CHECK (Authorization)
        if ((exe.Header.Flags & CXFlags.FLAG_KERNEL_PRIV) != 0)
            return "AUTHORIZED: Kernel/Executive Privilege Granted.";

        return "AUTHORIZED: Standard User Privilege Granted.";
    }
}