// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Buffers.Binary;
using System.Text;

namespace CXEX.Uefi.SecureBoot;

/// <summary>One variable in an EDK2 authenticated variable store.</summary>
public sealed class EfiVariable
{
    public required string Name { get; init; }
    public required Guid VendorGuid { get; init; }
    public required uint Attributes { get; init; }
    public required byte[] Data { get; init; }
    public DateTime? TimeStamp { get; init; }

    public const uint NonVolatile       = 0x01;
    public const uint BootServiceAccess = 0x02;
    public const uint RuntimeAccess     = 0x04;
    public const uint TimeBasedAuth     = 0x20;

    /// <summary>NV|BS|RT|TIME_BASED_AUTH - what PK, KEK, db and dbx all carry.</summary>
    public const uint SecureBootDbAttributes = NonVolatile | BootServiceAccess | RuntimeAccess | TimeBasedAuth;

    /// <summary>NV|BS - the firmware-private flags, not visible at runtime.</summary>
    public const uint SetupAttributes = NonVolatile | BootServiceAccess;
}

/// <summary>
/// Reads and writes the EDK2 variable store inside an OVMF <c>*_VARS.fd</c>.
/// </summary>
/// <remarks>
/// <para>Layout, all little-endian:</para>
/// <para>
/// 0x00 EFI_FIRMWARE_VOLUME_HEADER - ZeroVector[16], FileSystemGuid,
///      FvLength, '_FVH', Attributes, HeaderLength, Checksum, ExtHeaderOffset,
///      Reserved, Revision, then a block map terminated by {0,0}. HeaderLength
///      says where the volume header ends; it is 0x48 in practice but is read,
///      not assumed.
/// </para>
/// <para>
/// +HeaderLength VARIABLE_STORE_HEADER - Signature GUID, Size, Format (0x5A),
///      State (0xFE), then 6 reserved bytes. 28 bytes, so variables begin at
///      HeaderLength + 28 (0x64 for a stock OVMF store).
/// </para>
/// <para>
/// Then AUTHENTICATED_VARIABLE_HEADER records, followed by the UTF-16 name
/// (including its NUL) and the data, each record padded to a 4-byte boundary with
/// 0xFF. A StartId that is not 0x55AA means end of variables. The header is 60
/// bytes, laid out:
/// </para>
/// <code>
///   0x00 UINT16   StartId          0x55AA
///   0x02 UINT8    State            0x3F = VAR_ADDED
///   0x03 UINT8    Reserved
///   0x04 UINT32   Attributes
///   0x08 UINT64   MonotonicCount
///   0x10 EFI_TIME TimeStamp        16 bytes
///   0x20 UINT32   PubKeyIndex
///   0x24 UINT32   NameSize         bytes, including the UTF-16 NUL
///   0x28 UINT32   DataSize
///   0x2C EFI_GUID VendorGuid
/// </code>
/// <para>
/// Those offsets are written out because getting TimeStamp wrong is invisible: a
/// reader and writer that agree on the wrong offset round-trip perfectly, and
/// firmware does not validate the timestamp of a variable it finds already in the
/// store. The symptom only shows up in another implementation, as a year of 65535
/// read out of the 0xFF fill.
/// </para>
/// <para>
/// Only the authenticated store is supported. A plain store uses a shorter
/// 32-byte header and cannot hold PK/KEK/db at all, so accepting one would
/// produce a file that looks fine and never enforces anything.
/// </para>
/// <para>
/// Writing only ever touches the variable region. The FV header (and its
/// checksum) is preserved byte-for-byte, as is everything past the store - on a
/// 4 MB OVMF that tail is the fault-tolerant-write work and spare blocks, and
/// corrupting it makes firmware repair the store on boot and discard the keys.
/// </para>
/// </remarks>
public sealed class EfiVarStore
{
    private readonly byte[] _image;
    private readonly int _storeOffset;   // start of VARIABLE_STORE_HEADER
    private readonly int _bodyOffset;    // first variable
    private readonly int _bodyEnd;       // exclusive

    /// <summary>Variables in store order. Mutate, then call <see cref="Serialize"/>.</summary>
    public List<EfiVariable> Variables { get; } = [];

    private EfiVarStore(byte[] image, int storeOffset, int bodyOffset, int bodyEnd)
    {
        _image = image;
        _storeOffset = storeOffset;
        _bodyOffset = bodyOffset;
        _bodyEnd = bodyEnd;
    }

    public static EfiVarStore Load(string path) => Parse(File.ReadAllBytes(path));

    public static EfiVarStore Parse(byte[] image)
    {
        if (image.Length < 0x100)
            throw new InvalidDataException("file is too small to be an OVMF variable store");

        var fvGuid = new Guid(image.AsSpan(0x10, 16).ToArray());
        if (fvGuid != EfiGuids.SystemNvDataFv)
            throw new InvalidDataException(
                $"not an NV data firmware volume (GUID {fvGuid}); expected {EfiGuids.SystemNvDataFv}. " +
                "Point this at OVMF_VARS*.fd, not at the CODE image.");

        uint fvhSig = BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(0x28));
        if (fvhSig != 0x4856465F)   // '_FVH'
            throw new InvalidDataException("missing _FVH signature in the firmware volume header");

        int headerLength = BinaryPrimitives.ReadUInt16LittleEndian(image.AsSpan(0x30));
        if (headerLength < 0x40 || headerLength > image.Length - 28)
            throw new InvalidDataException($"implausible FV HeaderLength 0x{headerLength:X}");

        var storeGuid = new Guid(image.AsSpan(headerLength, 16).ToArray());
        if (storeGuid == EfiGuids.Variable)
            throw new InvalidDataException(
                "this is a PLAIN variable store, not an authenticated one. It cannot hold PK/KEK/db, " +
                "so Secure Boot would never be enforced. Use an OVMF build with Secure Boot support.");
        if (storeGuid != EfiGuids.AuthenticatedVariable)
            throw new InvalidDataException($"unrecognised variable store signature {storeGuid}");

        uint storeSize = BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(headerLength + 16));
        byte format = image[headerLength + 20];
        byte state = image[headerLength + 21];
        if (format != 0x5A)
            throw new InvalidDataException($"variable store Format is 0x{format:X2}, expected 0x5A (formatted)");
        if (state != 0xFE)
            throw new InvalidDataException($"variable store State is 0x{state:X2}, expected 0xFE (healthy)");

        int bodyOffset = headerLength + 28;
        long bodyEnd = headerLength + (long)storeSize;
        if (bodyEnd > image.Length)
            throw new InvalidDataException($"variable store claims to end at 0x{bodyEnd:X}, past end of file");

        var store = new EfiVarStore(image, headerLength, bodyOffset, (int)bodyEnd);
        store.ParseVariables();
        return store;
    }

    private void ParseVariables()
    {
        int at = _bodyOffset;
        while (at + 60 <= _bodyEnd)
        {
            ushort startId = BinaryPrimitives.ReadUInt16LittleEndian(_image.AsSpan(at));
            if (startId != 0x55AA) break;             // erased space; no more variables

            byte state = _image[at + 2];
            uint attrs = BinaryPrimitives.ReadUInt32LittleEndian(_image.AsSpan(at + 4));
            uint nameSize = BinaryPrimitives.ReadUInt32LittleEndian(_image.AsSpan(at + 0x24));
            uint dataSize = BinaryPrimitives.ReadUInt32LittleEndian(_image.AsSpan(at + 0x28));
            var guid = new Guid(_image.AsSpan(at + 0x2C, 16).ToArray());

            long total = 60L + nameSize + dataSize;
            if (at + total > _bodyEnd)
                throw new InvalidDataException($"variable at 0x{at:X} runs past the end of the store");

            // State 0x3F is VAR_ADDED. Anything else is a deleted or in-flight
            // record; it still occupies space and must be stepped over, but it
            // is not a live variable and is dropped on rewrite.
            if (state == 0x3F)
            {
                string name = Encoding.Unicode.GetString(_image, at + 60, (int)nameSize).TrimEnd('\0');
                var data = _image.AsSpan(at + 60 + (int)nameSize, (int)dataSize).ToArray();
                Variables.Add(new EfiVariable
                {
                    Name = name, VendorGuid = guid, Attributes = attrs, Data = data,
                    TimeStamp = ReadEfiTime(_image.AsSpan(at + 0x10, 16)),
                });
            }

            at += (int)total;
            at = (at + 3) & ~3;                        // HEADER_ALIGN is 4
        }
    }

    /// <summary>Adds a variable, replacing any existing one with the same name and vendor GUID.</summary>
    public void Set(EfiVariable v)
    {
        Variables.RemoveAll(x => x.Name == v.Name && x.VendorGuid == v.VendorGuid);
        Variables.Add(v);
    }

    public void Delete(string name, Guid vendor)
        => Variables.RemoveAll(x => x.Name == name && x.VendorGuid == vendor);

    public EfiVariable? Get(string name, Guid vendor)
        => Variables.FirstOrDefault(x => x.Name == name && x.VendorGuid == vendor);

    /// <summary>
    /// Rebuilds the image with the current variable list. Everything outside the
    /// variable region - the FV header, the FTW spare area past the store - is
    /// carried over untouched.
    /// </summary>
    public byte[] Serialize()
    {
        var outBuf = (byte[])_image.Clone();
        // Erased flash reads as 0xFF, and the variable driver treats 0xFF as free
        // space. Zeroing instead would make firmware read a StartId of 0x0000 and
        // still stop, but it would no longer match what a real store looks like.
        outBuf.AsSpan(_bodyOffset, _bodyEnd - _bodyOffset).Fill(0xFF);

        int at = _bodyOffset;
        foreach (var v in Variables)
        {
            byte[] name = Encoding.Unicode.GetBytes(v.Name + "\0");
            int total = 60 + name.Length + v.Data.Length;
            int padded = (total + 3) & ~3;
            if (at + padded > _bodyEnd)
                throw new InvalidOperationException(
                    $"variables do not fit: '{v.Name}' needs {padded} bytes, " +
                    $"{_bodyEnd - at} left in a {_bodyEnd - _bodyOffset}-byte store");

            var s = outBuf.AsSpan(at);
            BinaryPrimitives.WriteUInt16LittleEndian(s, 0x55AA);        // StartId
            s[2] = 0x3F;                                                // State: VAR_ADDED
            s[3] = 0x00;                                                // Reserved
            BinaryPrimitives.WriteUInt32LittleEndian(s[4..], v.Attributes);
            BinaryPrimitives.WriteUInt64LittleEndian(s[8..], 0);         // MonotonicCount
            WriteEfiTime(s.Slice(0x10, 16), v.TimeStamp);
            BinaryPrimitives.WriteUInt32LittleEndian(s[0x20..], 0);      // PubKeyIndex
            BinaryPrimitives.WriteUInt32LittleEndian(s[0x24..], (uint)name.Length);
            BinaryPrimitives.WriteUInt32LittleEndian(s[0x28..], (uint)v.Data.Length);
            v.VendorGuid.ToByteArray().CopyTo(s[0x2C..]);
            name.CopyTo(s[60..]);
            v.Data.CopyTo(s[(60 + name.Length)..]);
            // bytes between total and padded stay 0xFF from the fill above

            at += padded;
        }
        return outBuf;
    }

    public void Save(string path) => File.WriteAllBytes(path, Serialize());

    /// <summary>Bytes still free in the variable region, given the current list.</summary>
    public int FreeSpace()
    {
        int used = Variables.Sum(v =>
            (60 + Encoding.Unicode.GetBytes(v.Name + "\0").Length + v.Data.Length + 3) & ~3);
        return (_bodyEnd - _bodyOffset) - used;
    }

    // EFI_TIME: Year u16, Month, Day, Hour, Minute, Second, Pad1, Nanosecond u32,
    // TimeZone i16, Daylight, Pad2. 16 bytes. Firmware records it for
    // time-based-authenticated writes; an offline-authored variable just carries it.
    private static void WriteEfiTime(Span<byte> dst, DateTime? t)
    {
        dst.Clear();
        if (t is null) return;
        var u = t.Value.ToUniversalTime();
        BinaryPrimitives.WriteUInt16LittleEndian(dst, (ushort)u.Year);
        dst[2] = (byte)u.Month;
        dst[3] = (byte)u.Day;
        dst[4] = (byte)u.Hour;
        dst[5] = (byte)u.Minute;
        dst[6] = (byte)u.Second;
    }

    private static DateTime? ReadEfiTime(ReadOnlySpan<byte> src)
    {
        ushort year = BinaryPrimitives.ReadUInt16LittleEndian(src);
        if (year == 0) return null;
        try
        {
            return new DateTime(year, src[2], src[3], src[4], src[5], src[6], DateTimeKind.Utc);
        }
        catch (ArgumentOutOfRangeException)
        {
            return null;    // a garbage timestamp is not worth failing a parse over
        }
    }
}
