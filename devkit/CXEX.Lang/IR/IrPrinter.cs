// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace CXEX.Lang.IR;

/// <summary>
/// The IR as text: what <c>cxk ir</c> prints, and what the golden tests pin.
/// The grammar is in IR.md. Deterministic: the same module prints the same
/// bytes on every host.
/// </summary>
public static class IrPrinter
{
    public static string Print(IrModule m)
    {
        var sb = new StringBuilder();
        sb.Append("module pointer ").Append(m.PointerBytes).Append('\n');
        foreach (var s in m.Strings)
            sb.Append("string @").Append(s.Name).Append(" = \"").Append(Escape(s.Bytes)).Append("\"\n");
        foreach (var g in m.Globals) Global(sb, g);
        foreach (var f in m.Functions) Function(sb, f);
        return sb.ToString();
    }

    private static string Escape(byte[] bytes)
    {
        var sb = new StringBuilder();
        foreach (var b in bytes)
        {
            if (b >= 0x20 && b < 0x7F && b != '"' && b != '\\') sb.Append((char)b);
            else sb.Append('\\').Append(b.ToString("x2"));
        }
        return sb.ToString();
    }

    private static void Global(StringBuilder sb, IrGlobal g)
    {
        sb.Append("global @").Append(g.Name).Append(" size ").Append(g.Size).Append(" align ").Append(g.Align);
        if (g.Section != null) sb.Append(" section \"").Append(g.Section).Append('"');
        if (g.Init == null) { sb.Append(" zero\n"); return; }
        sb.Append(" = { ");
        sb.Append(string.Join(", ", g.Init.Select(d => d switch
        {
            IrDataBytes b => "bytes " + string.Join(' ', b.Bytes.Select(x => x.ToString())),
            IrDataAddr a => "addr @" + a.Symbol,
            IrDataZero z => "zero " + z.Size,
            _ => "?",
        })));
        sb.Append(" }\n");
    }

    private static string Param(IrParam p) =>
        (p.AggSize > 0 ? $"agg {p.AggSize}" : IrTypes.Name(p.Type)) + (p.Name.Length > 0 ? " " + p.Name : "");

    private static void Function(StringBuilder sb, IrFunction f)
    {
        sb.Append('\n').Append(f.IsExtern ? "extern fn @" : "fn @").Append(f.Name).Append('(');
        for (int i = 0; i < f.Params.Count; i++)
        {
            if (i > 0) sb.Append(", ");
            sb.Append(Param(f.Params[i]));
            if (!f.IsExtern) sb.Append(" %").Append(f.ParamRegs[i].Id);
        }
        sb.Append(") -> ").Append(Param(f.Return));
        if (f.Section != null) sb.Append(" section \"").Append(f.Section).Append('"');
        if (f.IsExtern) { sb.Append('\n'); return; }
        sb.Append(" {\n");
        foreach (var s in f.Slots)
        {
            sb.Append("  slot $").Append(s.Id).Append(' ').Append(s.Size).Append(" align ").Append(s.Align);
            if (s.Name != null) sb.Append(" ; ").Append(s.Name);
            sb.Append('\n');
        }
        foreach (var b in f.Blocks)
        {
            sb.Append('b').Append(b.Id).Append(":\n");
            foreach (var i in b.Instrs) sb.Append("  ").Append(Instr(i)).Append('\n');
            sb.Append("  ").Append(b.Term == null ? "<no terminator>" : Term(b.Term)).Append('\n');
        }
        sb.Append("}\n");
    }

    public static IrType TypeOf(IrValue v) => v switch
    {
        IrReg r => r.Type,
        IrConst c => c.Type,
        _ => IrType.Ptr,
    };

    public static string V(IrValue v) => v switch
    {
        IrReg r => "%" + r.Id,
        IrConst c => c.Value.ToString(),
        IrSym s => "@" + s.Name,
        IrSlotRef s => "$" + s.Id,
        _ => "?",
    };

    private static string TV(IrValue v) => IrTypes.Name(TypeOf(v)) + " " + V(v);
    private static string Lower(Enum e) => e.ToString().ToLowerInvariant();

    public static string Instr(IrInstr i) => i switch
    {
        IrBinary b => $"%{b.Dst.Id} = {Lower(b.Op)} {IrTypes.Name(b.Dst.Type)} {V(b.A)}, {V(b.B)}",
        IrUnary u => $"%{u.Dst.Id} = {Lower(u.Op)} {IrTypes.Name(u.Dst.Type)} {V(u.A)}",
        IrCompare c => $"%{c.Dst.Id} = cmp {Lower(c.Cc)} {IrTypes.Name(TypeOf(c.A))} {V(c.A)}, {V(c.B)}",
        IrConvert c => $"%{c.Dst.Id} = {Lower(c.Kind)} {TV(c.A)} to {IrTypes.Name(c.Dst.Type)}",
        IrLoad l => $"%{l.Dst.Id} = load {IrTypes.Name(l.Dst.Type)}, {V(l.Addr)}",
        IrStore s => $"store {TV(s.Value)}, {V(s.Addr)}",
        IrCopy c => $"copy {V(c.Dst)}, {V(c.Src)}, {c.Size}",
        IrZero z => $"zero {V(z.Dst)}, {z.Size}",
        IrPtrAdd p => $"%{p.Dst.Id} = ptradd {V(p.Base)}, {V(p.Offset)}",
        IrCall c => Call(c),
        IrSyscall s => $"%{s.Dst.Id} = syscall {IrTypes.Name(s.Dst.Type)} ({string.Join(", ", s.Args.Select(TV))})",
        IrAtomic a => Atomic(a),
        _ => "?",
    };

    /* `%d = atomic cas i32 %e, %n, %p`: the operands, then the address, as
       a store puts them; a store has no result. */
    private static string Atomic(IrAtomic a)
    {
        string ops = "";
        if (a.A != null) ops += V(a.A) + ", ";
        if (a.B != null) ops += V(a.B) + ", ";
        string body = $"atomic {Lower(a.Op)} {IrTypes.Name(a.Type)} {ops}{V(a.Addr)}";
        return a.Dst != null ? $"%{a.Dst.Id} = {body}" : body;
    }

    private static string Call(IrCall c)
    {
        string args = string.Join(", ", c.Args.Select(a => a.AggSize > 0 ? $"agg {a.AggSize} {V(a.Value)}" : TV(a.Value)));
        string call = $"call {V(c.Callee)}({args})";
        if (c.Dst != null) return $"%{c.Dst.Id} = {call} -> {IrTypes.Name(c.Dst.Type)}";
        if (c.AggDst != null) return $"{call} -> agg {c.AggSize} {V(c.AggDst)}";
        return call;
    }

    public static string Term(IrTerm t) => t switch
    {
        IrJump j => $"br b{j.Target}",
        IrBranch b => $"brif {V(b.Cond)}, b{b.Then}, b{b.Else}",
        IrSwitch s => $"switch {TV(s.Value)}, b{s.Default} [{string.Join(", ", s.Cases.Select(c => $"{c.Value}: b{c.Target}"))}]",
        IrRet r => r.Value == null ? "ret" : $"ret {TV(r.Value)}",
        IrRetAgg r => $"ret agg {r.Size} {V(r.Src)}",
        _ => "?",
    };
}
