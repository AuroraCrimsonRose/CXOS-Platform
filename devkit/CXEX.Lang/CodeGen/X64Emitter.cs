// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using CXEX.Lang.IR;

namespace CXEX.Lang.CodeGen;

/// <summary>
/// The x86-64 back end (roadmap step 6b3): the typed IR (IR.md) to GNU-syntax
/// assembly for the System V AMD64 ABI, which clang assembles and ld.lld
/// links into an ELF64.
///
/// <para>It reads IR.md, never the tree: every decision about width, sign and
/// order was made by the lowering, and all that is left here is choosing
/// instructions. It chooses them the simplest way that is right. Every
/// virtual register has a home in the frame, written once by the instruction
/// that defines it, and every instruction loads its operands from homes into
/// scratch registers (rax, rcx, rdx, rsi, rdi, r8-r11), works, and stores.
/// No callee-saved register is ever touched, so nothing needs saving. A
/// register allocator is an optimisation for later, on the IR.</para>
///
/// <para>A value narrower than 64 bits is kept in the low bits of its home;
/// the bits above are unspecified, and every instruction that could see them
/// (compare, divide, shift, widen, store) works at the value's own width.</para>
///
/// <para>Calls follow System V, in <see cref="Assign"/> for both sides: six
/// integer registers, an i128 in two of them, an aggregate of up to 16 bytes
/// in one or two (X has no floating point, so every eightbyte is INTEGER), a
/// larger one copied onto the stack; a result in rax (rdx), or for an
/// aggregate over 16 bytes through a pointer the caller passes in rdi.</para>
/// </summary>
public sealed class X64Emitter
{
    private readonly IrModule _m;
    private readonly StringBuilder _o = new();
    private bool _usesDiv128;

    /* As X86Emitter: `compile --object` exports every function and global
       and has no entry point. */
    public bool Library { get; init; }

    /* Enter the kernel through the system page (CX_SYSCALLS_IPC.md §4, §9.5):
       the loader hands _start the base in rsi, _start keeps it in
       __cx_sysbase, and a syscall calls the stub at base + n * 16 with the
       arguments in rdi, rsi, rdx, r10 and r8. Off for a program built
       without the prelude - the Linux host - where it is Linux's own
       `syscall`, with the same registers. */
    public bool SyscallPage { get; init; } = true;
    public const int SyscallStubShift = 4;   // log2 SYSPAGE_STUB_SIZE

    private static readonly string[] ArgRegs = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
    private static readonly string[] SyscallRegs = { "rdi", "rsi", "rdx", "r10", "r8" };

    public X64Emitter(IrModule m)
    {
        if (m.PointerBytes != 8) throw new ArgumentException("the x86-64 back end needs a module lowered for 8-byte pointers");
        _m = m;
    }

    public string Emit()
    {
        L(".text");
        if (!Library) L(".globl _start");
        foreach (var f in _m.Functions)
            if (!f.IsExtern) EmitFn(f);
        if (_usesDiv128) EmitDiv128();

        foreach (var g in _m.Globals) EmitGlobal(g);
        foreach (var s in _m.Strings)
        {
            /* Strings live in .data, as the x86-32 emitter puts them. */
            L(".data");
            L($"{s.Name}:");
            Bytes(s.Bytes);
        }
        if (SyscallPage && !Library)
        {
            L(".bss");
            L("    .balign 8");
            L("__cx_sysbase:");
            L("    .zero 8");
        }
        return _o.ToString();
    }

    // ---- data ----
    private void EmitGlobal(IrGlobal g)
    {
        if (g.Section != null) L($".section {g.Section},\"aw\",@progbits");   // allocated + writable
        else L(g.Init == null ? ".bss" : ".data");
        L($"    .balign {g.Align}");
        if (Library) L($".globl {g.Name}");
        L($"{g.Name}:");
        if (g.Init == null) { L($"    .zero {g.Size}"); return; }
        foreach (var d in g.Init)
            switch (d)
            {
                case IrDataBytes b: Bytes(b.Bytes); break;
                case IrDataAddr a: L($"    .quad {a.Symbol}"); break;
                case IrDataZero z: L($"    .zero {z.Size}"); break;
            }
    }

    private void Bytes(byte[] b)
    {
        for (int i = 0; i < b.Length; i += 16)
            L("    .byte " + string.Join(",", b.Skip(i).Take(16)));
    }

    // ---- the frame ----
    private IrFunction _f = null!;
    private Dictionary<int, int> _home = null!;   // register id -> rbp offset
    private int[] _slot = null!;                  // slot id -> rbp offset
    private int _stage, _scratch, _sret;          // the call staging area, a 16-byte scratch, the result pointer
    private bool _hasSret;

    /* Where one argument goes: in Words registers from Reg, or (Reg < 0) at
       Stack bytes into the argument area, which starts at the caller's rsp
       and so at the callee's 16(%rbp). */
    private readonly record struct ArgLoc(int Reg, int Words, int Stack, int Size);

    /* The System V assignment, one function for caller and callee so the two
       cannot disagree. An argument that does not fit in the registers left
       goes on the stack whole, and a later one that fits still takes a
       register. */
    private static List<ArgLoc> Assign(IEnumerable<(IrType Type, int Agg)> args, bool sret, out int stackBytes)
    {
        var locs = new List<ArgLoc>();
        int reg = sret ? 1 : 0, stack = 0;
        foreach (var (t, agg) in args)
        {
            int words = agg > 0 ? (agg + 7) / 8 : t == IrType.I128 ? 2 : 1;
            bool memory = agg > 16;
            int size = agg > 0 ? agg : words * 8;
            if (!memory && reg + words <= ArgRegs.Length)
            {
                locs.Add(new ArgLoc(reg, words, 0, size));
                reg += words;
                continue;
            }
            stack = Round(stack, agg == 0 && t == IrType.I128 ? 16 : 8);
            locs.Add(new ArgLoc(-1, words, stack, size));
            stack += Round(size, 8);
        }
        stackBytes = Round(stack, 16);
        return locs;
    }

    private static int Round(int n, int a) => (n + a - 1) / a * a;
    private bool ReturnsViaPointer(IrFunction f) => f.Return.AggSize > 16;

    private void EmitFn(IrFunction f)
    {
        _f = f;
        _home = new();
        _hasSret = ReturnsViaPointer(f);
        int size = 0;
        int Alloc(int n, int align) { size = Round(size + n, align); return -size; }

        _slot = new int[f.Slots.Count];
        foreach (var s in f.Slots) _slot[s.Id] = Alloc(s.Size, Math.Max(s.Align, 1));
        void Home(IrReg r) { if (!_home.ContainsKey(r.Id)) _home[r.Id] = r.Type == IrType.I128 ? Alloc(16, 16) : Alloc(8, 8); }
        foreach (var r in f.ParamRegs) Home(r);
        foreach (var b in f.Blocks)
            foreach (var i in b.Instrs)
                if (DstOf(i) is IrReg d) Home(d);
        _stage = Alloc(8 * ArgRegs.Length, 8);
        _scratch = Alloc(16, 16);
        _sret = Alloc(8, 8);

        var locs = Assign(f.Params.Select(p => (p.Type, p.AggSize)), _hasSret, out _);
        var copies = new int[f.Params.Count];
        for (int i = 0; i < f.Params.Count; i++)
            if (f.Params[i].AggSize > 0 && locs[i].Reg >= 0) copies[i] = Alloc(locs[i].Words * 8, 8);
        int frame = Round(size, 16);

        L(f.Section != null ? $".section {f.Section},\"ax\",@progbits" : ".text");   // allocated + executable
        if (Library) L($".globl {f.Name}");
        L($"{f.Name}:");
        bool entry = !Library && f.Name == "_start";
        if (entry && SyscallPage) T("movq %rsi, __cx_sysbase(%rip)");   // before anything uses rsi
        T("pushq %rbp");
        T("movq %rsp, %rbp");
        if (frame > 0) T($"subq ${frame}, %rsp");
        /* Nothing calls _start: the stack is 16-aligned at entry, so after
           the push it is not, and a call from here would be misaligned. */
        if (entry) T("andq $-16, %rsp");
        if (_hasSret) T($"movq %rdi, {_sret}(%rbp)");

        for (int i = 0; i < f.Params.Count; i++)
        {
            var p = f.Params[i];
            var loc = locs[i];
            var r = f.ParamRegs[i];
            if (p.AggSize > 0)
            {
                /* The function's own copy: what came in registers, spilled
                   to the frame; what came on the stack is the callee's
                   already. */
                if (loc.Reg >= 0)
                {
                    for (int w = 0; w < loc.Words; w++) T($"movq %{ArgRegs[loc.Reg + w]}, {copies[i] + 8 * w}(%rbp)");
                    T($"leaq {copies[i]}(%rbp), %rax");
                }
                else T($"leaq {16 + loc.Stack}(%rbp), %rax");
                Put(r, "rax");
                continue;
            }
            if (loc.Reg >= 0)
            {
                T($"movq %{ArgRegs[loc.Reg]}, {H(r)}");
                if (loc.Words == 2) T($"movq %{ArgRegs[loc.Reg + 1]}, {H(r, 8)}");
            }
            else
            {
                T($"movq {16 + loc.Stack}(%rbp), %rax");
                Put(r, "rax");
                if (loc.Words == 2) { T($"movq {16 + loc.Stack + 8}(%rbp), %rax"); PutHi(r, "rax"); }
            }
        }

        for (int bi = 0; bi < f.Blocks.Count; bi++)
        {
            var b = f.Blocks[bi];
            L($"{Label(b.Id)}:");
            foreach (var i in b.Instrs) EmitInstr(i);
            EmitTerm(b.Term!, bi + 1 < f.Blocks.Count ? f.Blocks[bi + 1].Id : -1);
        }
    }

    private string Label(int block) => $".L{_f.Name}.b{block}";

    private static IrReg? DstOf(IrInstr i) => i switch
    {
        IrBinary x => x.Dst, IrUnary x => x.Dst, IrCompare x => x.Dst, IrConvert x => x.Dst,
        IrLoad x => x.Dst, IrPtrAdd x => x.Dst, IrCall x => x.Dst, IrSyscall x => x.Dst,
        IrAtomic x => x.Dst, IrTlsBase x => x.Dst,
        _ => null,
    };

    // ---- operands ----
    private string H(IrReg r, int plus = 0) => $"{_home[r.Id] + plus}(%rbp)";

    private static IrType TypeOf(IrValue v) => v switch
    {
        IrReg r => r.Type,
        IrConst c => c.Type,
        _ => IrType.Ptr,
    };

    /* The low 64 bits of v into a register. Touches no other register. */
    private void Get(IrValue v, string reg)
    {
        switch (v)
        {
            case IrReg r: T($"movq {H(r)}, %{reg}"); break;
            case IrConst c: Const((ulong)(c.Value & ulong.MaxValue), reg); break;
            case IrSym s: T($"leaq {s.Name}(%rip), %{reg}"); break;
            case IrSlotRef s: T($"leaq {_slot[s.Id]}(%rbp), %{reg}"); break;
        }
    }

    /* The high 64 bits of an i128 into a register. */
    private void GetHi(IrValue v, string reg)
    {
        switch (v)
        {
            case IrReg { Type: IrType.I128 } r: T($"movq {H(r, 8)}, %{reg}"); break;
            case IrConst c: Const((ulong)(c.Value >> 64), reg); break;
            default: Const(0, reg); break;
        }
    }

    private void Const(ulong v, string reg)
    {
        if (v == 0) T($"xorl {R(reg, 4)}, {R(reg, 4)}");
        else if (v <= uint.MaxValue) T($"movl ${v}, {R(reg, 4)}");   // zero-extends
        else T($"movabsq ${v}, %{reg}");
    }

    private void Put(IrReg d, string reg) => T($"movq %{reg}, {H(d)}");
    private void PutHi(IrReg d, string reg) => T($"movq %{reg}, {H(d, 8)}");

    /* A scratch register by width: R("rax", 1) is %al. */
    private static string R(string reg, int bytes)
    {
        if (reg.StartsWith("r") && char.IsDigit(reg[1]))
            return "%" + reg + bytes switch { 1 => "b", 2 => "w", 4 => "d", _ => "" };
        string x = reg.Substring(1);   // "ax", "cx", "si" ...
        return "%" + bytes switch
        {
            1 => x is "si" or "di" ? x + "l" : x[0] + "l",
            2 => x,
            4 => "e" + x,
            _ => reg,
        };
    }

    private int Bytes(IrType t) => IrTypes.Bytes(t, 8);
    private static string Suffix(int bytes) => bytes switch { 1 => "b", 2 => "w", 4 => "l", _ => "q" };

    // ---- instructions ----
    private void EmitInstr(IrInstr i)
    {
        switch (i)
        {
            case IrBinary b when b.Dst.Type == IrType.I128: Binary128(b); break;
            case IrBinary b: Binary(b); break;
            case IrUnary u: Unary(u); break;
            case IrCompare c: Compare(c); break;
            case IrConvert c: Convert(c); break;

            case IrLoad ld:
                Get(ld.Addr, "rcx");
                switch (ld.Dst.Type)
                {
                    case IrType.Bool or IrType.I8: T("movzbl (%rcx), %eax"); break;
                    case IrType.I16: T("movzwl (%rcx), %eax"); break;
                    case IrType.I32: T("movl (%rcx), %eax"); break;
                    default: T("movq (%rcx), %rax"); break;
                }
                Put(ld.Dst, "rax");
                if (ld.Dst.Type == IrType.I128) { T("movq 8(%rcx), %rax"); PutHi(ld.Dst, "rax"); }
                break;

            case IrStore st:
            {
                var t = TypeOf(st.Value);
                Get(st.Addr, "rcx");
                Get(st.Value, "rax");
                int n = Bytes(t);
                T($"mov{Suffix(Math.Min(n, 8))} {R("rax", Math.Min(n, 8))}, (%rcx)");
                if (t == IrType.I128) { GetHi(st.Value, "rax"); T("movq %rax, 8(%rcx)"); }
                break;
            }

            case IrCopy cp:
                Get(cp.Dst, "rdi");
                Get(cp.Src, "rsi");
                T($"movl ${cp.Size}, %ecx");
                T("rep movsb");
                break;

            case IrZero z:
                Get(z.Dst, "rdi");
                T($"movl ${z.Size}, %ecx");
                T("xorl %eax, %eax");
                T("rep stosb");
                break;

            case IrPtrAdd pa:
                Get(pa.Base, "rax");
                Get(pa.Offset, "rcx");
                T("addq %rcx, %rax");
                Put(pa.Dst, "rax");
                break;

            case IrCall c: Call(c); break;
            case IrSyscall s: Syscall(s); break;
            case IrAtomic a: Atomic(a); break;
            /* The word at FS_BASE + 0, the block's address of itself: one
               load, with no FSGSBASE (rdfsbase) on the FX-8350. */
            case IrTlsBase t: T("movq %fs:0, %rax"); Put(t.Dst, "rax"); break;
            default: throw new InvalidOperationException($"the x86-64 back end has no rule for {i.GetType().Name}");
        }
    }

    private void Binary(IrBinary b)
    {
        var t = b.Dst.Type;
        int n = Bytes(t);
        Get(b.A, "rax");
        Get(b.B, "rcx");
        switch (b.Op)
        {
            /* The low bits of + - * & | ^ do not depend on the bits above. */
            case IrBinOp.Add: T("addq %rcx, %rax"); break;
            case IrBinOp.Sub: T("subq %rcx, %rax"); break;
            case IrBinOp.Mul: T("imulq %rcx, %rax"); break;
            case IrBinOp.And: T("andq %rcx, %rax"); break;
            case IrBinOp.Or: T("orq %rcx, %rax"); break;
            case IrBinOp.Xor: T("xorq %rcx, %rax"); break;

            case IrBinOp.SDiv or IrBinOp.SRem or IrBinOp.UDiv or IrBinOp.URem:
            {
                bool signed = b.Op is IrBinOp.SDiv or IrBinOp.SRem;
                if (n == 8)
                {
                    T(signed ? "cqto" : "xorl %edx, %edx");
                    T(signed ? "idivq %rcx" : "divq %rcx");
                }
                else
                {
                    if (n < 4) { Extend("rax", n, signed); Extend("rcx", n, signed); }
                    /* Below 32 bits the most negative value over -1 fits in a
                       32-bit idiv, which would not trap; IR.md says it does. */
                    if (n < 4 && signed)
                    {
                        string ok = NewLocal();
                        T("cmpl $-1, %ecx");
                        T($"jne {ok}");
                        T($"cmpl ${-(1 << (n * 8 - 1))}, %eax");
                        T($"jne {ok}");
                        T("ud2");
                        L($"{ok}:");
                    }
                    T(signed ? "cltd" : "xorl %edx, %edx");
                    T(signed ? "idivl %ecx" : "divl %ecx");
                }
                if (b.Op is IrBinOp.SRem or IrBinOp.URem) T("movq %rdx, %rax");
                break;
            }

            case IrBinOp.Shl or IrBinOp.LShr or IrBinOp.AShr:
            {
                /* The count is taken modulo the width. The CPU masks it to 5
                   bits at 32 and 6 at 64, which is that; narrower types mask
                   it here and shift a value widened by the right sign. */
                string op = b.Op switch { IrBinOp.Shl => "shl", IrBinOp.LShr => "shr", _ => "sar" };
                if (n == 8) { T($"{op}q %cl, %rax"); break; }
                if (n < 4)
                {
                    T($"andl ${n * 8 - 1}, %ecx");
                    if (b.Op != IrBinOp.Shl) Extend("rax", n, b.Op == IrBinOp.AShr);
                }
                T($"{op}l %cl, %eax");
                break;
            }
        }
        Put(b.Dst, "rax");
    }

    /* Widen the low n bytes of a register to 32 bits by the sign asked for. */
    private void Extend(string reg, int n, bool signed)
    {
        string op = (signed ? "movs" : "movz") + (n == 1 ? "bl" : "wl");
        T($"{op} {R(reg, n)}, {R(reg, 4)}");
    }

    private void Binary128(IrBinary b)
    {
        Get(b.A, "rax"); GetHi(b.A, "rdx");
        Get(b.B, "rcx"); GetHi(b.B, "rsi");
        switch (b.Op)
        {
            case IrBinOp.Add: T("addq %rcx, %rax"); T("adcq %rsi, %rdx"); break;
            case IrBinOp.Sub: T("subq %rcx, %rax"); T("sbbq %rsi, %rdx"); break;
            case IrBinOp.And: T("andq %rcx, %rax"); T("andq %rsi, %rdx"); break;
            case IrBinOp.Or: T("orq %rcx, %rax"); T("orq %rsi, %rdx"); break;
            case IrBinOp.Xor: T("xorq %rcx, %rax"); T("xorq %rsi, %rdx"); break;
            case IrBinOp.Mul:
                /* (ah·2^64 + al)(bh·2^64 + bl) mod 2^128
                   = al·bl + (ah·bl + al·bh)·2^64 */
                T("movq %rdx, %r8");
                T("imulq %rcx, %r8");
                T("movq %rsi, %r9");
                T("imulq %rax, %r9");
                T("addq %r9, %r8");
                T("mulq %rcx");
                T("addq %r8, %rdx");
                break;
            case IrBinOp.SDiv or IrBinOp.SRem or IrBinOp.UDiv or IrBinOp.URem:
                _usesDiv128 = true;
                T(b.Op is IrBinOp.SDiv or IrBinOp.SRem ? "call __cx_sdivmod128" : "call __cx_udivmod128");
                if (b.Op is IrBinOp.SRem or IrBinOp.URem) { T("movq %r8, %rax"); T("movq %r9, %rdx"); }
                break;
            case IrBinOp.Shl or IrBinOp.LShr or IrBinOp.AShr:
            {
                /* shld/shrd and the shift take the count modulo 64; a count
                   of 64 or more then moves one half into the other. */
                string done = NewLocal();
                T("andl $127, %ecx");
                if (b.Op == IrBinOp.Shl)
                {
                    T("shldq %cl, %rax, %rdx");
                    T("shlq %cl, %rax");
                    T("testb $64, %cl");
                    T($"je {done}");
                    T("movq %rax, %rdx");
                    T("xorl %eax, %eax");
                }
                else
                {
                    bool arith = b.Op == IrBinOp.AShr;
                    T("shrdq %cl, %rdx, %rax");
                    T(arith ? "sarq %cl, %rdx" : "shrq %cl, %rdx");
                    T("testb $64, %cl");
                    T($"je {done}");
                    T("movq %rdx, %rax");
                    T(arith ? "sarq $63, %rdx" : "xorl %edx, %edx");
                }
                L($"{done}:");
                break;
            }
        }
        Put(b.Dst, "rax");
        PutHi(b.Dst, "rdx");
    }

    private void Unary(IrUnary u)
    {
        Get(u.A, "rax");
        if (u.Dst.Type != IrType.I128)
        {
            T(u.Op == IrUnOp.Neg ? "negq %rax" : "notq %rax");
            Put(u.Dst, "rax");
            return;
        }
        GetHi(u.A, "rdx");
        if (u.Op == IrUnOp.Neg)
        {
            T("negq %rax");        // CF: the low half was not zero
            T("adcq $0, %rdx");
            T("negq %rdx");
        }
        else { T("notq %rax"); T("notq %rdx"); }
        Put(u.Dst, "rax");
        PutHi(u.Dst, "rdx");
    }

    private static string Cc(IrCmp c) => c switch
    {
        IrCmp.Eq => "e", IrCmp.Ne => "ne",
        IrCmp.Slt => "l", IrCmp.Sle => "le", IrCmp.Sgt => "g", IrCmp.Sge => "ge",
        IrCmp.Ult => "b", IrCmp.Ule => "be", IrCmp.Ugt => "a", _ => "ae",
    };

    private void Compare(IrCompare c)
    {
        var t = TypeOf(c.A);
        if (t != IrType.I128)
        {
            int n = Bytes(t);
            Get(c.A, "rax");
            Get(c.B, "rcx");
            T($"cmp{Suffix(n)} {R("rcx", n)}, {R("rax", n)}");   // a - b
            T($"set{Cc(c.Cc)} %al");
        }
        else if (c.Cc is IrCmp.Eq or IrCmp.Ne)
        {
            Get(c.A, "rax"); GetHi(c.A, "rdx");
            Get(c.B, "rcx"); GetHi(c.B, "rsi");
            T("xorq %rcx, %rax");
            T("xorq %rsi, %rdx");
            T("orq %rdx, %rax");
            T($"set{Cc(c.Cc)} %al");
        }
        else
        {
            /* The flags of x - y across both halves give x < y: CF unsigned,
               SF != OF signed. a > b and a <= b are b < a and its negation. */
            bool swap = c.Cc is IrCmp.Sgt or IrCmp.Sle or IrCmp.Ugt or IrCmp.Ule;
            var (x, y) = swap ? (c.B, c.A) : (c.A, c.B);
            Get(x, "rax"); GetHi(x, "rdx");
            Get(y, "rcx"); GetHi(y, "rsi");
            T("cmpq %rcx, %rax");
            T("sbbq %rsi, %rdx");
            string cc = c.Cc switch
            {
                IrCmp.Slt or IrCmp.Sgt => "l",
                IrCmp.Sge or IrCmp.Sle => "ge",
                IrCmp.Ult or IrCmp.Ugt => "b",
                _ => "ae",
            };
            T($"set{cc} %al");
        }
        T("movzbl %al, %eax");
        Put(c.Dst, "rax");
    }

    private void Convert(IrConvert c)
    {
        var from = TypeOf(c.A);
        var to = c.Dst.Type;
        Get(c.A, "rax");
        switch (c.Kind)
        {
            case IrConv.Trunc:
                if (to == IrType.Bool) T("andl $1, %eax");
                break;
            case IrConv.ZExt:
                switch (from)
                {
                    case IrType.Bool or IrType.I8: T("movzbl %al, %eax"); break;
                    case IrType.I16: T("movzwl %ax, %eax"); break;
                    case IrType.I32: T("movl %eax, %eax"); break;
                }
                if (to == IrType.I128) { Put(c.Dst, "rax"); Const(0, "rdx"); PutHi(c.Dst, "rdx"); return; }
                break;
            case IrConv.SExt:
                switch (from)
                {
                    case IrType.I8: T("movsbq %al, %rax"); break;
                    case IrType.I16: T("movswq %ax, %rax"); break;
                    case IrType.I32: T("movslq %eax, %rax"); break;
                }
                if (to == IrType.I128)
                {
                    Put(c.Dst, "rax");
                    T("movq %rax, %rdx");
                    T("sarq $63, %rdx");
                    PutHi(c.Dst, "rdx");
                    return;
                }
                break;
            /* PtrToInt and IntToPtr: the same 64 bits. */
        }
        Put(c.Dst, "rax");
    }

    private void Call(IrCall c)
    {
        int agg = c.AggDst != null ? c.AggSize : 0;
        bool sret = agg > 16;
        var locs = Assign(c.Args.Select(a => (TypeOf(a.Value), a.AggSize)), sret, out int stackBytes);

        /* The stack arguments first, then every register word staged in the
           frame, then the registers loaded together: rep movsb would clobber
           rdi, rsi and rcx if a register were loaded before a copy. */
        if (stackBytes > 0) T($"subq ${stackBytes}, %rsp");
        int regs = sret ? 1 : 0;
        for (int i = 0; i < c.Args.Count; i++)
        {
            var a = c.Args[i];
            var loc = locs[i];
            if (loc.Reg >= 0) regs = Math.Max(regs, loc.Reg + loc.Words);
            if (a.AggSize > 0)
            {
                Get(a.Value, "rsi");
                if (loc.Reg >= 0) T($"leaq {_stage + 8 * loc.Reg}(%rbp), %rdi");
                else T($"leaq {loc.Stack}(%rsp), %rdi");
                T($"movl ${a.AggSize}, %ecx");
                T("rep movsb");
                continue;
            }
            Get(a.Value, "rax");
            string at = loc.Reg >= 0 ? $"{_stage + 8 * loc.Reg}(%rbp)" : $"{loc.Stack}(%rsp)";
            T($"movq %rax, {at}");
            if (loc.Words == 2)
            {
                GetHi(a.Value, "rax");
                T($"movq %rax, {(loc.Reg >= 0 ? $"{_stage + 8 * loc.Reg + 8}(%rbp)" : $"{loc.Stack + 8}(%rsp)")}");
            }
        }
        if (sret) { Get(c.AggDst!, "rax"); T($"movq %rax, {_stage}(%rbp)"); }
        for (int r = 0; r < regs; r++) T($"movq {_stage + 8 * r}(%rbp), %{ArgRegs[r]}");

        if (c.Callee is IrSym s) T($"call {s.Name}");
        else { Get(c.Callee, "r11"); T("call *%r11"); }
        if (stackBytes > 0) T($"addq ${stackBytes}, %rsp");

        if (c.Dst != null)
        {
            Put(c.Dst, "rax");
            if (c.Dst.Type == IrType.I128) PutHi(c.Dst, "rdx");
        }
        else if (agg > 0 && !sret)
        {
            /* Up to 16 bytes came back in rax and rdx. */
            T($"movq %rax, {_scratch}(%rbp)");
            T($"movq %rdx, {_scratch + 8}(%rbp)");
            Get(c.AggDst!, "rdi");
            T($"leaq {_scratch}(%rbp), %rsi");
            T($"movl ${agg}, %ecx");
            T("rep movsb");
        }
    }

    /* Sequentially consistent on x86-64 (the mapping C11 compilers use): a
       load is a plain mov, and everything that writes is a locked
       instruction - xchg with memory locks by itself - so no load or store
       moves across it. A cmpxchg that succeeds leaves rax as it was, the
       expected value; no reader of a 4-byte value trusts the upper half of
       its home (a zext clears it), so that needs nothing. */
    private void Atomic(IrAtomic a)
    {
        int n = Bytes(a.Type);
        string s = Suffix(n);
        Get(a.Addr, "rcx");
        if (a.A != null) Get(a.A, "rax");
        if (a.B != null) Get(a.B, "rdx");
        switch (a.Op)
        {
            case IrAtomicOp.Load: T($"mov{s} (%rcx), {R("rax", n)}"); break;
            case IrAtomicOp.Store:
            case IrAtomicOp.Xchg: T($"xchg{s} {R("rax", n)}, (%rcx)"); break;
            case IrAtomicOp.Add: T($"lock xadd{s} {R("rax", n)}, (%rcx)"); break;
            case IrAtomicOp.Cas: T($"lock cmpxchg{s} {R("rdx", n)}, (%rcx)"); break;
        }
        if (a.Dst != null) Put(a.Dst, "rax");
    }

    private void Syscall(IrSyscall s)
    {
        for (int i = 0; i < SyscallRegs.Length; i++)
        {
            if (i + 1 < s.Args.Count) Get(s.Args[i + 1], SyscallRegs[i]);
            else Const(0, SyscallRegs[i]);
        }
        Get(s.Args[0], "rax");
        if (SyscallPage)
        {
            T($"shlq ${SyscallStubShift}, %rax");
            T("addq __cx_sysbase(%rip), %rax");
            T("call *%rax");   // the stub puts the number in eax itself
        }
        else T("syscall");
        Put(s.Dst, "rax");
    }

    // ---- terminators ----
    private void EmitTerm(IrTerm t, int next)
    {
        switch (t)
        {
            case IrJump j:
                if (j.Target != next) T($"jmp {Label(j.Target)}");
                break;
            case IrBranch br:
                Get(br.Cond, "rax");
                T("testb %al, %al");
                T($"jne {Label(br.Then)}");
                if (br.Else != next) T($"jmp {Label(br.Else)}");
                break;
            case IrSwitch sw:
            {
                var ty = TypeOf(sw.Value);
                int n = Bytes(ty);
                Get(sw.Value, "rax");
                if (ty == IrType.I128) GetHi(sw.Value, "rdx");
                foreach (var (v, target) in sw.Cases)
                {
                    var k = new IrConst(ty, v);
                    Get(k, "rcx");
                    if (ty != IrType.I128)
                    {
                        T($"cmp{Suffix(n)} {R("rcx", n)}, {R("rax", n)}");
                        T($"je {Label(target)}");
                        continue;
                    }
                    string miss = NewLocal();
                    GetHi(k, "rsi");
                    T("cmpq %rcx, %rax");
                    T($"jne {miss}");
                    T("cmpq %rsi, %rdx");
                    T($"je {Label(target)}");
                    L($"{miss}:");
                }
                if (sw.Default != next) T($"jmp {Label(sw.Default)}");
                break;
            }
            case IrRet r:
                if (r.Value != null)
                {
                    Get(r.Value, "rax");
                    if (TypeOf(r.Value) == IrType.I128) GetHi(r.Value, "rdx");
                }
                T("leave");
                T("ret");
                break;
            case IrRetAgg ra:
                Get(ra.Src, "rsi");
                if (_hasSret)
                {
                    T($"movq {_sret}(%rbp), %rdi");
                    T($"movl ${ra.Size}, %ecx");
                    T("rep movsb");
                    T($"movq {_sret}(%rbp), %rax");   // System V hands the pointer back
                }
                else
                {
                    T($"leaq {_scratch}(%rbp), %rdi");
                    T($"movl ${ra.Size}, %ecx");
                    T("rep movsb");
                    T($"movq {_scratch}(%rbp), %rax");
                    T($"movq {_scratch + 8}(%rbp), %rdx");
                }
                T("leave");
                T("ret");
                break;
        }
    }

    /* i128 divide and remainder, emitted once when used. Not System V:
       the dividend comes in rdx:rax, the divisor in rsi:rcx; the quotient
       goes back in rdx:rax and the remainder in r9:r8. A zero divisor
       faults as a native divide does (#DE). */
    private void EmitDiv128()
    {
        L(".text");
        L("__cx_udivmod128:");
        T("movq %rcx, %rdi");
        T("orq %rsi, %rdi");
        T("jnz 1f");
        T("divl %edi");               // by zero: #DE
        L("1:");
        T("xorl %r8d, %r8d");
        T("xorl %r9d, %r9d");
        T("movl $128, %r10d");
        /* Shift-subtract, one quotient bit a round: shift the dividend's top
           bit into the remainder, and subtract the divisor when it fits. The
           shift never carries out of the remainder: before it, the remainder
           is at most the dividend's bits read so far, below 2^127. */
        L("2:");
        T("addq %rax, %rax");
        T("adcq %rdx, %rdx");
        T("adcq %r8, %r8");
        T("adcq %r9, %r9");
        T("movq %r8, %r11");
        T("movq %r9, %rdi");
        T("subq %rcx, %r11");
        T("sbbq %rsi, %rdi");
        T("jb 3f");
        T("movq %r11, %r8");
        T("movq %rdi, %r9");
        T("orq $1, %rax");
        L("3:");
        T("decl %r10d");
        T("jnz 2b");
        T("ret");

        /* Signed: divide the magnitudes; the quotient is negative when the
           signs differ, the remainder takes the dividend's. The most negative
           value over -1 has no quotient, and traps. */
        L("__cx_sdivmod128:");
        T("movq %rcx, %rdi");
        T("andq %rsi, %rdi");
        T("cmpq $-1, %rdi");
        T("jne 1f");
        T("testq %rax, %rax");
        T("jne 1f");
        T("movabsq $0x8000000000000000, %rdi");
        T("cmpq %rdi, %rdx");
        T("jne 1f");
        T("ud2");
        L("1:");
        T("movq %rdx, %r10");
        T("sarq $63, %r10");          // the dividend's sign: 0 or -1
        T("movq %rsi, %r11");
        T("sarq $63, %r11");          // the divisor's
        T("pushq %r10");
        T("pushq %r11");
        T("xorq %r10, %rax");         // |x| = (x ^ s) - s
        T("xorq %r10, %rdx");
        T("subq %r10, %rax");
        T("sbbq %r10, %rdx");
        T("xorq %r11, %rcx");
        T("xorq %r11, %rsi");
        T("subq %r11, %rcx");
        T("sbbq %r11, %rsi");
        T("call __cx_udivmod128");
        T("popq %r11");
        T("popq %r10");
        T("xorq %r10, %r11");         // the quotient's sign
        T("xorq %r11, %rax");
        T("xorq %r11, %rdx");
        T("subq %r11, %rax");
        T("sbbq %r11, %rdx");
        T("xorq %r10, %r8");
        T("xorq %r10, %r9");
        T("subq %r10, %r8");
        T("sbbq %r10, %r9");
        T("ret");
    }

    private int _locals;
    private string NewLocal() => $".Lx{_locals++}";

    private void L(string s) => _o.Append(s).Append('\n');
    private void T(string s) => _o.Append("    ").Append(s).Append('\n');
}
