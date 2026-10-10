// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace CXEX.Uefi.SecureBoot;

/// <summary>
/// Generates and loads a Secure Boot key set, and enrolls it into a variable store.
/// </summary>
/// <remarks>
/// <para>The three keys are not interchangeable:</para>
/// <list type="bullet">
/// <item>PK - one per machine, owns the platform. Enrolling a PK is what takes
/// firmware out of Setup Mode, and holding it is what allows KEK to be replaced.</item>
/// <item>KEK - authorises updates to db and dbx.</item>
/// <item>db - the list firmware checks a binary against. THIS is the key that
/// signs BOOTX64.EFI.</item>
/// </list>
/// <para>
/// Three keys rather than one reused three times, because db is the one that
/// touches binaries and is therefore the most exposed; it has to be replaceable
/// without reflashing the platform.
/// </para>
/// <para>
/// A leaked PK is not a leaked test key. It lets anyone sign a bootloader that an
/// enrolled machine will trust for as long as that PK is enrolled.
/// </para>
/// </remarks>
public static class SecureBootKeys
{
    public enum Role { Pk, Kek, Db }

    /// <summary>
    /// Creates a self-signed RSA certificate suitable for a Secure Boot role.
    /// </summary>
    /// <remarks>
    /// UEFI matches db entries by certificate, not by extended key usage, so the
    /// EKU below is not what makes verification work - it is there so the cert
    /// looks correct to Windows tooling that does check it.
    /// </remarks>
    public static X509Certificate2 Create(Role role, string organisation, int bits = 2048, int years = 10)
    {
        string cn = role switch
        {
            Role.Pk  => "CXK Secure Boot PK",
            Role.Kek => "CXK Secure Boot KEK",
            Role.Db  => "CXK Secure Boot db",
            _ => throw new ArgumentOutOfRangeException(nameof(role)),
        };

        using var rsa = RSA.Create(bits);
        var req = new CertificateRequest(
            new X500DistinguishedName($"CN={cn}, O={organisation}"),
            rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);

        // PK and KEK sign other keys; db signs images. Marking PK/KEK as CAs
        // matches what real platforms carry and keeps chain-checking tools happy.
        bool isCa = role != Role.Db;
        req.CertificateExtensions.Add(new X509BasicConstraintsExtension(isCa, false, 0, true));
        req.CertificateExtensions.Add(new X509KeyUsageExtension(
            isCa ? X509KeyUsageFlags.KeyCertSign | X509KeyUsageFlags.DigitalSignature
                 : X509KeyUsageFlags.DigitalSignature, true));
        if (role == Role.Db)
            req.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(
                [new Oid("1.3.6.1.5.5.7.3.3", "Code Signing")], false));
        req.CertificateExtensions.Add(new X509SubjectKeyIdentifierExtension(req.PublicKey, false));

        var now = DateTimeOffset.UtcNow;
        // Backdated a day so a signature made immediately is not rejected by a
        // verifier whose clock is behind - and firmware clocks frequently are.
        return req.CreateSelfSigned(now.AddDays(-1), now.AddYears(years));
    }

    /// <summary>Writes a keypair as PFX (private, for signing), DER and PEM (public, for enrolling).</summary>
    public static void Export(X509Certificate2 cert, string basePath, string? pfxPassword)
    {
        File.WriteAllBytes(basePath + ".pfx", cert.Export(X509ContentType.Pkcs12, pfxPassword));
        File.WriteAllBytes(basePath + ".cer", cert.Export(X509ContentType.Cert));   // DER, for firmware menus
        File.WriteAllText(basePath + ".pem", cert.ExportCertificatePem() + Environment.NewLine);
    }

    public static X509Certificate2 LoadPfx(string path, string? password)
        => X509CertificateLoader.LoadPkcs12(File.ReadAllBytes(path), password,
               X509KeyStorageFlags.EphemeralKeySet);

    /// <summary>Loads a certificate from PEM, DER or PFX, by content rather than extension.</summary>
    public static X509Certificate2 LoadCertificate(string path)
    {
        byte[] raw = File.ReadAllBytes(path);
        var text = System.Text.Encoding.ASCII.GetString(raw, 0, Math.Min(raw.Length, 64));
        if (text.Contains("-----BEGIN"))
            return X509Certificate2.CreateFromPem(File.ReadAllText(path));
        return X509CertificateLoader.LoadCertificate(raw);
    }

    /// <summary>
    /// Enrolls PK, KEK and db into a store and turns Secure Boot on.
    /// </summary>
    /// <param name="store">A store parsed from a stock OVMF_VARS image.</param>
    /// <param name="owner">
    /// Owner GUID recorded against each entry. A label only - firmware never
    /// verifies it; it exists so a machine holding several vendors' keys can tell
    /// them apart.
    /// </param>
    /// <remarks>
    /// Microsoft's keys are deliberately NOT added. With them also enrolled, a
    /// failure to verify our own signature could be masked by anything else
    /// Microsoft-signed on the same ESP, so a successful boot would no longer
    /// prove our signature was checked.
    /// </remarks>
    public static void Enroll(EfiVarStore store, X509Certificate2 pk, X509Certificate2 kek,
                              X509Certificate2 db, Guid owner)
    {
        var now = DateTime.UtcNow;

        void SetDb(string name, Guid vendor, byte[] payload) => store.Set(new EfiVariable
        {
            Name = name, VendorGuid = vendor, Attributes = EfiVariable.SecureBootDbAttributes,
            Data = payload, TimeStamp = now,
        });

        SetDb("db",  EfiGuids.ImageSecurityDatabase, EfiSignatureList.FromCertificate(db.RawData, owner));
        SetDb("dbx", EfiGuids.ImageSecurityDatabase, EfiSignatureList.EmptyDbx());
        SetDb("KEK", EfiGuids.GlobalVariable,        EfiSignatureList.FromCertificate(kek.RawData, owner));
        // PK last of the four: it is the one that ends Setup Mode, so the rest
        // should already be in place when firmware reads it.
        SetDb("PK",  EfiGuids.GlobalVariable,        EfiSignatureList.FromCertificate(pk.RawData, owner));

        // CustomMode 0 = STANDARD_SECURE_BOOT_MODE. Leaving it at 1 would let
        // anything change the key databases without authorisation, which is not
        // what we are testing.
        store.Set(new EfiVariable
        {
            Name = "CustomMode", VendorGuid = EfiGuids.CustomModeEnable,
            Attributes = EfiVariable.SetupAttributes, Data = [0],
        });
        store.Set(new EfiVariable
        {
            Name = "SecureBootEnable", VendorGuid = EfiGuids.SecureBootEnableDisable,
            Attributes = EfiVariable.SetupAttributes, Data = [1],
        });
    }
}
