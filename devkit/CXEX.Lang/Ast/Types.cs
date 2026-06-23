namespace CXEX.Lang.Ast;

/// <summary>Static type references as written in source (resolved/checked in Sema).</summary>
public abstract record TypeRef;

public enum PrimKind { I8, I16, I32, U8, U16, U32, Bool, Void }

public sealed record PrimType(PrimKind Kind) : TypeRef;
public sealed record PointerType(TypeRef Pointee) : TypeRef;        // *T
public sealed record ArrayType(TypeRef Element, int Length) : TypeRef; // [N]T
public sealed record NamedType(string Name) : TypeRef;             // struct name / alias