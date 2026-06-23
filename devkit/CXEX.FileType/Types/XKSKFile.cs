using System.IO;
using System.Text;
using CXEX.Core.Interfaces;

namespace CXEX.FileType.Types;

public class XKSKFile : ICXFile
{
    public string PemData { get; private set; } = string.Empty;

    public string GetDisplayName() => "XKSK Cryptographic Private Key";

    public void Load(byte[] data)
    {
        PemData = Encoding.ASCII.GetString(data);

        if (!PemData.Contains("PRIVATE KEY"))
            throw new InvalidDataException("Target does not contain valid PEM private key material.");
    }
}