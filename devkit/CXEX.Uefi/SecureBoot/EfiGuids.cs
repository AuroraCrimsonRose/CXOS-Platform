namespace CXEX.Uefi.SecureBoot;

/// <summary>
/// The GUIDs the Secure Boot variable store is built out of.
/// </summary>
/// <remarks>
/// EFI_GUID is { UINT32, UINT16, UINT16, UINT8[8] } with the first three fields
/// little-endian, which is exactly the mixed-endian layout .NET's
/// <see cref="Guid.ToByteArray()"/> produces. So these round-trip to firmware
/// byte-for-byte with no swapping - do not "fix" that by reversing anything.
/// </remarks>
public static class EfiGuids
{
    /// <summary>Marks the firmware volume that holds the NV variable store.</summary>
    public static readonly Guid SystemNvDataFv = new("fff12b8d-7696-4c8b-a985-2747075b4f50");

    /// <summary>Variable store signature for an AUTHENTICATED store. Secure Boot needs this one.</summary>
    public static readonly Guid AuthenticatedVariable = new("aaf32c78-947b-439a-a180-2e144ec37792");

    /// <summary>Variable store signature for a plain store; cannot hold PK/KEK/db.</summary>
    public static readonly Guid Variable = new("ddcf3616-3275-4164-98b6-fe85707ffe7d");

    /// <summary>Vendor GUID for PK and KEK.</summary>
    public static readonly Guid GlobalVariable = new("8be4df61-93ca-11d2-aa0d-00e098032b8c");

    /// <summary>Vendor GUID for db and dbx. NOT the global one - a common mistake.</summary>
    public static readonly Guid ImageSecurityDatabase = new("d719b2cb-3d3a-4596-a3bc-dad00e67656f");

    /// <summary>Vendor GUID for SecureBootEnable.</summary>
    public static readonly Guid SecureBootEnableDisable = new("f0a30bc7-af08-4556-99c4-001009c93a44");

    /// <summary>Vendor GUID for CustomMode.</summary>
    public static readonly Guid CustomModeEnable = new("c076ec0c-7028-4399-a072-71ee5c448b9f");

    /// <summary>Signature list type: the entry is a DER X.509 certificate.</summary>
    public static readonly Guid CertX509 = new("a5c059a1-94e4-4aa7-87b5-ab155c2bf072");

    /// <summary>Signature list type: the entry is a raw SHA-256 digest.</summary>
    public static readonly Guid CertSha256 = new("c1c41626-504c-4092-aca9-41f936934328");

    /// <summary>
    /// The owner GUID EDK2 tooling conventionally uses for the placeholder dbx
    /// entry. Not special to firmware - an owner GUID is a label, never verified.
    /// </summary>
    public static readonly Guid DummyOwner = new("a0baa8a3-041d-48a8-bc87-c36d121b5e3d");
}
