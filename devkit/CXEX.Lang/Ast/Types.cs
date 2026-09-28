using System.Collections.Generic;
namespace CXEX.Lang.Ast;

/// <summary>Static type references as written in source (resolved/checked in Sema).</summary>
public abstract record TypeRef;

/// <summary>
/// Integer widths X knows natively.
///
/// Ordered narrow-to-wide within each signedness so width comparisons are a
/// table lookup rather than a switch - adding U256 later is a row in
/// PrimWidth.Bytes and nothing else. That is the future-proofing that costs
/// nothing: the MECHANISM is width-parameterised, so the set of types can grow
/// without the emitter's value model changing.
///
/// The line stops at 128 deliberately. Up to 128 bits a value is ONE thing -
/// an offset, a timestamp, a GUID, a Q64.64 coordinate, a widening product -
/// that you compare, add and pass by value. Past 128 it is a buffer with
/// operations: nobody adds two RSA moduli or orders two SHA digests by
/// magnitude, they feed them to an algorithm. That is a library, and bignum.c
/// already is one.
/// </summary>
public enum PrimKind { I8, I16, I32, I64, I128, U8, U16, U32, U64, U128, Bool, Void }

/// <summary>Width of each primitive, in bytes. One place, so a new width is one row.</summary>
public static class PrimWidth
{
    public static int Bytes(PrimKind k) => k switch
    {
        PrimKind.I8 or PrimKind.U8 or PrimKind.Bool => 1,
        PrimKind.I16 or PrimKind.U16 => 2,
        PrimKind.I64 or PrimKind.U64 => 8,
        PrimKind.I128 or PrimKind.U128 => 16,
        PrimKind.Void => 0,
        _ => 4,
    };

    /// <summary>True for a type that does not fit in one machine register.</summary>
    public static bool IsWide(PrimKind k) => Bytes(k) > 4;

    public static bool IsSigned(PrimKind k) =>
        k is PrimKind.I8 or PrimKind.I16 or PrimKind.I32 or PrimKind.I64 or PrimKind.I128;
}

public sealed record PrimType(PrimKind Kind) : TypeRef;
public sealed record PointerType(TypeRef Pointee) : TypeRef;        // *T
public sealed record ArrayType(TypeRef Element, int Length) : TypeRef; // [N]T
public sealed record NamedType(string Name) : TypeRef;             // struct name / alias
public sealed record FuncType(List<TypeRef> Params, TypeRef Return) : TypeRef;  // fn(T,U) -> R             // struct name / alias