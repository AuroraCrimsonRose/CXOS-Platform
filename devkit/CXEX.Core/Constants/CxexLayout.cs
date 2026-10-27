// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
namespace CXEX.Core.Constants;

/// <summary>
/// The CXEX container's fixed sizes, format version 2 (<c>kernel/lib/format/cxex.h</c>
/// holds the kernel's copy). The writer, the parser and the tests take them from
/// here rather than each restating 88.
/// </summary>
public static class CxexLayout
{
    /// <summary>The header: 56 bytes of fields, then the ABI hash.</summary>
    public const int HeaderSize = 88;

    /// <summary>Where the ABI hash sits: SHA-256 of the <c>.xfil</c> the image was built against.</summary>
    public const int AbiHashOffset = 56;

    /// <summary>The ABI hash's length.</summary>
    public const int AbiHashSize = 32;

    /// <summary>One section-table entry.</summary>
    public const int SectionSize = 28;
}
