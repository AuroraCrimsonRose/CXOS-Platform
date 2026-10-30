// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;

namespace CXEX.Lang.IR;

/* The typed IR (roadmap step 6b2, CX_X_PLAN.md §2).
 *
 * The x86-32 emitter writes assembly straight from the checked tree, and every
 * decision it makes - how wide a value is, which divide, which compare, what
 * order operands run in - lives in that tree walk. The IR writes those
 * decisions down once, so a back end only has to choose instructions: the
 * x86-64 System V one next (6b3), ARM later, and a register allocator and the
 * simple optimisations on this instead of on assembly text.
 *
 * The shape:
 *
 *   - A value has one of six types: bool, i8, i16, i32, i64, i128, ptr. An
 *     integer type is a width, not a sign; the operations that care (divide,
 *     remainder, right shift, ordering, widening) say which they mean. usize
 *     and isize are the integer as wide as ptr (Module.PointerBytes).
 *   - A struct, an array and a sum type are never values. They are bytes in
 *     memory, reached through a ptr, and moved with `copy`; passing or
 *     returning one by value is an `agg` argument or return, and the back end
 *     decides how its ABI does that.
 *   - Virtual registers (%n) are assigned exactly once. Variables are not
 *     registers: every local and parameter has a stack slot ($n) and is loaded
 *     and stored, as clang does at -O0. Promoting slots to registers is an
 *     optimisation for later, not something every back end must undo.
 *   - A function is basic blocks, each ending in exactly one terminator.
 *
 * The meaning of every operation is in IR.md beside this file. The lowering
 * keeps the x86-32 emitter's semantics exactly, including the order operands
 * are evaluated in, so the two can be held to each other. */

public enum IrType { Void, Bool, I8, I16, I32, I64, I128, Ptr }

public static class IrTypes
{
    /* Bits of a value. bool counts one: it holds 0 or 1, and widening it is a
       zero extension like any other narrower type. */
    public static int Bits(IrType t, int pointerBytes) => t switch
    {
        IrType.Bool => 1,
        IrType.I8 => 8,
        IrType.I16 => 16,
        IrType.I32 => 32,
        IrType.I64 => 64,
        IrType.I128 => 128,
        IrType.Ptr => pointerBytes * 8,
        _ => 0,
    };

    /* Bytes a value of the type takes in memory. */
    public static int Bytes(IrType t, int pointerBytes) => t switch
    {
        IrType.Bool or IrType.I8 => 1,
        IrType.I16 => 2,
        IrType.I32 => 4,
        IrType.I64 => 8,
        IrType.I128 => 16,
        IrType.Ptr => pointerBytes,
        _ => 0,
    };

    public static bool IsInt(IrType t) => t is IrType.I8 or IrType.I16 or IrType.I32 or IrType.I64 or IrType.I128;

    public static IrType IntOfBytes(int bytes) => bytes switch
    {
        1 => IrType.I8,
        2 => IrType.I16,
        4 => IrType.I32,
        8 => IrType.I64,
        16 => IrType.I128,
        _ => throw new ArgumentOutOfRangeException(nameof(bytes)),
    };

    public static string Name(IrType t) => t switch
    {
        IrType.Void => "void",
        IrType.Bool => "bool",
        IrType.Ptr => "ptr",
        _ => "i" + Bits(t, 0),
    };

    public static UInt128 Mask(int bits) => bits >= 128 ? UInt128.MaxValue : (UInt128.One << bits) - 1;
}

// ---- operands ----
public abstract record IrValue;
/// <summary>A virtual register, assigned once.</summary>
public sealed record IrReg(int Id, IrType Type) : IrValue;
/// <summary>A constant, held reduced to its type's width.</summary>
public sealed record IrConst(IrType Type, UInt128 Value) : IrValue;
/// <summary>The address of a function, a global or a string: a ptr.</summary>
public sealed record IrSym(string Name) : IrValue;
/// <summary>The address of one of the function's stack slots: a ptr.</summary>
public sealed record IrSlotRef(int Id) : IrValue;

// ---- instructions ----
public enum IrBinOp { Add, Sub, Mul, SDiv, UDiv, SRem, URem, And, Or, Xor, Shl, LShr, AShr }
public enum IrUnOp { Neg, Not }   // Not is bitwise: ~
public enum IrCmp { Eq, Ne, Slt, Sle, Sgt, Sge, Ult, Ule, Ugt, Uge }
public enum IrConv { Trunc, ZExt, SExt, PtrToInt, IntToPtr }

public abstract record IrInstr;
public sealed record IrBinary(IrReg Dst, IrBinOp Op, IrValue A, IrValue B) : IrInstr;
public sealed record IrUnary(IrReg Dst, IrUnOp Op, IrValue A) : IrInstr;
public sealed record IrCompare(IrReg Dst, IrCmp Cc, IrValue A, IrValue B) : IrInstr;
public sealed record IrConvert(IrReg Dst, IrConv Kind, IrValue A) : IrInstr;
public sealed record IrLoad(IrReg Dst, IrValue Addr) : IrInstr;
public sealed record IrStore(IrValue Value, IrValue Addr) : IrInstr;
public sealed record IrCopy(IrValue Dst, IrValue Src, int Size) : IrInstr;
public sealed record IrZero(IrValue Dst, int Size) : IrInstr;
public sealed record IrPtrAdd(IrReg Dst, IrValue Base, IrValue Offset) : IrInstr;
/// <summary>An argument: a value, or (AggSize &gt; 0) a copy of AggSize bytes at the address Value.</summary>
public sealed record IrArg(IrValue Value, int AggSize = 0);
/// <summary>
/// A call. Dst is null for a void or aggregate result; an aggregate result is
/// written to the AggSize bytes at AggDst.
/// </summary>
public sealed record IrCall(IrReg? Dst, IrValue Callee, List<IrArg> Args, IrValue? AggDst = null, int AggSize = 0) : IrInstr;
/// <summary>A system call: Args[0] is the number, then up to five arguments, every one a pointer-width integer.</summary>
public sealed record IrSyscall(IrReg Dst, List<IrValue> Args) : IrInstr;
/// <summary>
/// An atomic access to the Type-sized integer at Addr, sequentially consistent.
/// Load has no operand; Store, Xchg and Add take A; Cas takes A (expected) and
/// B (new). Dst is the old value, null for Store.
/// </summary>
public sealed record IrAtomic(IrReg? Dst, IrAtomicOp Op, IrType Type, IrValue Addr, IrValue? A = null, IrValue? B = null) : IrInstr;
public enum IrAtomicOp { Load, Store, Xchg, Add, Cas }
/// <summary>
/// The calling thread's thread-local block: the pointer-width word at the start
/// of it, which holds its own address (Sema/Intrinsics.cs, <c>__tls_base</c>).
/// </summary>
public sealed record IrTlsBase(IrReg Dst) : IrInstr;

// ---- terminators ----
public abstract record IrTerm;
public sealed record IrJump(int Target) : IrTerm;
public sealed record IrBranch(IrValue Cond, int Then, int Else) : IrTerm;
public sealed record IrSwitch(IrValue Value, int Default, List<(UInt128 Value, int Target)> Cases) : IrTerm;
public sealed record IrRet(IrValue? Value) : IrTerm;
public sealed record IrRetAgg(IrValue Src, int Size) : IrTerm;

// ---- structure ----
public sealed class IrSlot
{
    public int Id;
    public int Size;
    public int Align;
    public string? Name;   // the variable it holds; null for a temporary
}

public sealed class IrBlock
{
    public int Id;
    public readonly List<IrInstr> Instrs = new();
    public IrTerm? Term;
}

/// <summary>A parameter, or a return: a value type, or (AggSize &gt; 0) an aggregate of that many bytes.</summary>
public sealed record IrParam(string Name, IrType Type, int AggSize = 0);

public sealed class IrFunction
{
    public string Name = "";
    public string? Section;
    public readonly List<IrParam> Params = new();
    /* The register each parameter arrives in: its value, or for an aggregate
       a ptr to the function's own copy. */
    public readonly List<IrReg> ParamRegs = new();
    public IrParam Return = new("", IrType.Void);
    public readonly List<IrSlot> Slots = new();
    public readonly List<IrBlock> Blocks = new();   // Blocks[0] is the entry
    public bool IsExtern;
}

public abstract record IrData;
public sealed record IrDataBytes(byte[] Bytes) : IrData;
public sealed record IrDataAddr(string Symbol) : IrData;   // a ptr-sized address
public sealed record IrDataZero(int Size) : IrData;

public sealed class IrGlobal
{
    public string Name = "";
    public int Size;
    public int Align;
    public string? Section;
    public List<IrData>? Init;   // null: all zero
}

public sealed class IrString
{
    public string Name = "";
    public byte[] Bytes = Array.Empty<byte>();   // NUL included
}

public sealed class IrModule
{
    public int PointerBytes;
    public readonly List<IrFunction> Functions = new();
    public readonly List<IrGlobal> Globals = new();
    public readonly List<IrString> Strings = new();

    /* The integer as wide as a pointer: what usize and isize become, what
       ptrtoint yields and what an offset or a syscall word is. */
    public IrType PtrInt => IrTypes.IntOfBytes(PointerBytes);
}
