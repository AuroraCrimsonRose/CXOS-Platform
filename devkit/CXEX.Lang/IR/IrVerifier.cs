// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Linq;

namespace CXEX.Lang.IR;

/// <summary>
/// Check that a module is well formed, so a back end can rely on it instead
/// of re-checking: every block ends in one terminator that names blocks that
/// exist; every register is assigned once, by an instruction that runs before
/// every use (dominates it); every operand has the type its operation takes
/// (IR.md); every symbol and slot exists; a call to a known function matches
/// its signature. Returns the problems found, empty when there are none.
/// </summary>
public static class IrVerifier
{
    public static List<string> Verify(IrModule m)
    {
        var errs = new List<string>();
        var fns = new Dictionary<string, IrFunction>();
        var syms = new HashSet<string>();
        foreach (var f in m.Functions)
        {
            if (!syms.Add(f.Name)) errs.Add($"@{f.Name}: defined twice");
            fns[f.Name] = f;
        }
        foreach (var g in m.Globals)
        {
            if (!syms.Add(g.Name)) errs.Add($"@{g.Name}: defined twice");
            int n = 0;
            foreach (var d in g.Init ?? new List<IrData>())
            {
                n += d switch { IrDataBytes b => b.Bytes.Length, IrDataAddr => m.PointerBytes, IrDataZero z => z.Size, _ => 0 };
                if (d is IrDataZero { Size: <= 0 }) errs.Add($"@{g.Name}: zero of {((IrDataZero)d).Size} bytes");
            }
            if (g.Init != null && n != g.Size) errs.Add($"@{g.Name}: initializer is {n} bytes, the global {g.Size}");
        }
        foreach (var s in m.Strings)
        {
            if (!syms.Add(s.Name)) errs.Add($"@{s.Name}: defined twice");
            if (s.Bytes.Length == 0 || s.Bytes[^1] != 0) errs.Add($"@{s.Name}: not NUL-terminated");
        }
        foreach (var g in m.Globals)
            foreach (var d in g.Init ?? new List<IrData>())
                if (d is IrDataAddr a && !syms.Contains(a.Symbol)) errs.Add($"@{g.Name}: addr of unknown @{a.Symbol}");

        foreach (var f in m.Functions)
            if (!f.IsExtern) new FnCheck(m, f, fns, syms, errs).Run();
        return errs;
    }

    private sealed class FnCheck
    {
        private readonly IrModule _m;
        private readonly IrFunction _f;
        private readonly Dictionary<string, IrFunction> _fns;
        private readonly HashSet<string> _syms;
        private readonly List<string> _errs;
        private readonly Dictionary<int, (int Block, int Index, IrType Type)> _defs = new();
        private int _block, _index;

        public FnCheck(IrModule m, IrFunction f, Dictionary<string, IrFunction> fns, HashSet<string> syms, List<string> errs)
        { _m = m; _f = f; _fns = fns; _syms = syms; _errs = errs; }

        private void Err(string msg) => _errs.Add($"@{_f.Name} b{_block}: {msg}");
        private int Bits(IrType t) => IrTypes.Bits(t, _m.PointerBytes);

        public void Run()
        {
            if (_f.Blocks.Count == 0) { _errs.Add($"@{_f.Name}: no blocks"); return; }
            if (_f.ParamRegs.Count != _f.Params.Count) _errs.Add($"@{_f.Name}: {_f.Params.Count} params, {_f.ParamRegs.Count} registers");
            for (int i = 0; i < _f.Slots.Count; i++)
                if (_f.Slots[i].Id != i || _f.Slots[i].Size <= 0 || _f.Slots[i].Align <= 0) _errs.Add($"@{_f.Name}: slot {i} malformed");
            for (int i = 0; i < _f.Blocks.Count; i++)
                if (_f.Blocks[i].Id != i) _errs.Add($"@{_f.Name}: block {i} numbered {_f.Blocks[i].Id}");

            /* Definitions first: parameters at the entry, then every instruction. */
            for (int i = 0; i < _f.ParamRegs.Count && i < _f.Params.Count; i++)
            {
                var r = _f.ParamRegs[i];
                var want = _f.Params[i].AggSize > 0 ? IrType.Ptr : _f.Params[i].Type;
                if (r.Type != want) _errs.Add($"@{_f.Name}: param {i} register is {r.Type}, the param {want}");
                Define(r, 0, -1);
            }
            for (int b = 0; b < _f.Blocks.Count; b++)
                for (int i = 0; i < _f.Blocks[b].Instrs.Count; i++)
                    if (DstOf(_f.Blocks[b].Instrs[i]) is IrReg d) { _block = b; Define(d, b, i); }

            var idom = Dominators();
            for (_block = 0; _block < _f.Blocks.Count; _block++)
            {
                var blk = _f.Blocks[_block];
                for (_index = 0; _index < blk.Instrs.Count; _index++) Check(blk.Instrs[_index], idom);
                _index = blk.Instrs.Count;
                if (blk.Term == null) { Err("no terminator"); continue; }
                CheckTerm(blk.Term, idom);
            }
        }

        private void Define(IrReg r, int block, int index)
        {
            if (r.Type == IrType.Void) Err($"%{r.Id} has type void");
            if (!_defs.TryAdd(r.Id, (block, index, r.Type))) Err($"%{r.Id} assigned twice");
        }

        private static IrReg? DstOf(IrInstr i) => i switch
        {
            IrBinary x => x.Dst, IrUnary x => x.Dst, IrCompare x => x.Dst, IrConvert x => x.Dst,
            IrLoad x => x.Dst, IrPtrAdd x => x.Dst, IrCall x => x.Dst, IrSyscall x => x.Dst,
            IrAtomic x => x.Dst, IrTlsBase x => x.Dst,
            _ => null,
        };

        /* Immediate dominators by the iterative algorithm (Cooper, Harvey and
           Kennedy), on the blocks the entry reaches. -1: unreachable. */
        private int[] Dominators()
        {
            int n = _f.Blocks.Count;
            var preds = new List<int>[n];
            for (int i = 0; i < n; i++) preds[i] = new List<int>();
            for (int i = 0; i < n; i++)
                if (_f.Blocks[i].Term is IrTerm t)
                    foreach (int s in IrLowering.Successors(t))
                        if (s >= 0 && s < n) preds[s].Add(i);
            var order = new List<int>();   // reverse postorder
            var seen = new bool[n];
            void Dfs(int b)
            {
                seen[b] = true;
                if (_f.Blocks[b].Term is IrTerm t)
                    foreach (int s in IrLowering.Successors(t))
                        if (s >= 0 && s < n && !seen[s]) Dfs(s);
                order.Add(b);
            }
            Dfs(0);
            order.Reverse();
            var rpo = new int[n];
            for (int i = 0; i < order.Count; i++) rpo[order[i]] = i;
            var idom = Enumerable.Repeat(-1, n).ToArray();
            idom[0] = 0;
            bool changed = true;
            while (changed)
            {
                changed = false;
                foreach (int b in order.Skip(1))
                {
                    int nd = -1;
                    foreach (int p in preds[b])
                    {
                        if (idom[p] == -1) continue;
                        if (nd == -1) { nd = p; continue; }
                        int x = p, y = nd;
                        while (x != y)
                        {
                            while (rpo[x] > rpo[y]) x = idom[x];
                            while (rpo[y] > rpo[x]) y = idom[y];
                        }
                        nd = x;
                    }
                    if (nd != idom[b]) { idom[b] = nd; changed = true; }
                }
            }
            return idom;
        }

        private bool Dominates(int a, int b, int[] idom)
        {
            if (idom[b] == -1) return true;   // unreachable code dominates nothing and needs nothing
            while (true)
            {
                if (a == b) return true;
                if (b == 0) return false;
                b = idom[b];
            }
        }

        /* An operand: registers must be defined before this point; symbols and
           slots must exist. Returns its type. */
        private IrType Use(IrValue v, int[] idom)
        {
            switch (v)
            {
                case IrReg r:
                    if (!_defs.TryGetValue(r.Id, out var d)) { Err($"%{r.Id} is never assigned"); return r.Type; }
                    if (d.Type != r.Type) Err($"%{r.Id} used as {r.Type}, assigned as {d.Type}");
                    bool ok = d.Block == _block ? d.Index < _index : Dominates(d.Block, _block, idom);
                    if (!ok) Err($"%{r.Id} is used where its assignment does not always run first");
                    return r.Type;
                case IrConst c:
                    if (c.Type == IrType.Void) Err("a void constant");
                    else if ((c.Value & ~IrTypes.Mask(Bits(c.Type))) != 0) Err($"constant {c.Value} does not fit {IrTypes.Name(c.Type)}");
                    return c.Type;
                case IrSym s:
                    if (!_syms.Contains(s.Name)) Err($"unknown symbol @{s.Name}");
                    return IrType.Ptr;
                case IrSlotRef s:
                    if (s.Id < 0 || s.Id >= _f.Slots.Count) Err($"unknown slot ${s.Id}");
                    return IrType.Ptr;
            }
            Err("unknown operand");
            return IrType.Void;
        }

        private void Want(IrValue v, IrType t, int[] idom, string what)
        {
            var got = Use(v, idom);
            if (got != t) Err($"{what} is {IrTypes.Name(got)}, wants {IrTypes.Name(t)}");
        }

        private void Check(IrInstr i, int[] idom)
        {
            switch (i)
            {
                case IrBinary b:
                    if (!IrTypes.IsInt(b.Dst.Type)) Err($"{b.Op} on {IrTypes.Name(b.Dst.Type)}");
                    Want(b.A, b.Dst.Type, idom, $"{b.Op} operand");
                    Want(b.B, b.Dst.Type, idom, $"{b.Op} operand");
                    break;
                case IrUnary u:
                    if (!IrTypes.IsInt(u.Dst.Type)) Err($"{u.Op} on {IrTypes.Name(u.Dst.Type)}");
                    Want(u.A, u.Dst.Type, idom, $"{u.Op} operand");
                    break;
                case IrCompare c:
                    {
                        if (c.Dst.Type != IrType.Bool) Err("cmp result must be bool");
                        var t = Use(c.A, idom);
                        Want(c.B, t, idom, "cmp operand");
                        bool eq = c.Cc is IrCmp.Eq or IrCmp.Ne;
                        bool unsignedOrder = c.Cc is IrCmp.Ult or IrCmp.Ule or IrCmp.Ugt or IrCmp.Uge;
                        if (!(IrTypes.IsInt(t) || (t == IrType.Bool && (eq || unsignedOrder)) || (t == IrType.Ptr && (eq || unsignedOrder))))
                            Err($"cmp {c.Cc} on {IrTypes.Name(t)}");
                        break;
                    }
                case IrConvert c:
                    {
                        var from = Use(c.A, idom);
                        var to = c.Dst.Type;
                        bool ok = c.Kind switch
                        {
                            IrConv.Trunc => IrTypes.IsInt(from) && IrTypes.IsInt(to) && Bits(to) < Bits(from),
                            IrConv.ZExt => (IrTypes.IsInt(from) || from == IrType.Bool) && IrTypes.IsInt(to) && Bits(to) > Bits(from),
                            IrConv.SExt => IrTypes.IsInt(from) && IrTypes.IsInt(to) && Bits(to) > Bits(from),
                            IrConv.PtrToInt => from == IrType.Ptr && to == _m.PtrInt,
                            IrConv.IntToPtr => from == _m.PtrInt && to == IrType.Ptr,
                            _ => false,
                        };
                        if (!ok) Err($"{c.Kind} from {IrTypes.Name(from)} to {IrTypes.Name(to)}");
                        break;
                    }
                case IrLoad l:
                    Want(l.Addr, IrType.Ptr, idom, "load address");
                    break;
                case IrStore s:
                    if (Use(s.Value, idom) == IrType.Void) Err("store of void");
                    Want(s.Addr, IrType.Ptr, idom, "store address");
                    break;
                case IrCopy c:
                    Want(c.Dst, IrType.Ptr, idom, "copy destination");
                    Want(c.Src, IrType.Ptr, idom, "copy source");
                    if (c.Size <= 0) Err($"copy of {c.Size} bytes");
                    break;
                case IrZero z:
                    Want(z.Dst, IrType.Ptr, idom, "zero destination");
                    if (z.Size <= 0) Err($"zero of {z.Size} bytes");
                    break;
                case IrPtrAdd p:
                    if (p.Dst.Type != IrType.Ptr) Err("ptradd result must be ptr");
                    Want(p.Base, IrType.Ptr, idom, "ptradd base");
                    Want(p.Offset, _m.PtrInt, idom, "ptradd offset");
                    break;
                case IrCall c: CheckCall(c, idom); break;
                case IrSyscall s:
                    if (s.Args.Count < 1 || s.Args.Count > 6) Err($"syscall with {s.Args.Count} words");
                    foreach (var a in s.Args) Want(a, _m.PtrInt, idom, "syscall word");
                    if (!IrTypes.IsInt(s.Dst.Type)) Err("syscall result must be an integer");
                    break;
                case IrTlsBase t:
                    if (t.Dst.Type != _m.PtrInt) Err($"tlsbase result must be {IrTypes.Name(_m.PtrInt)}");
                    break;
                case IrAtomic a:
                    {
                        if (a.Type is not (IrType.I32 or IrType.I64)) Err($"atomic on {IrTypes.Name(a.Type)}");
                        Want(a.Addr, IrType.Ptr, idom, "atomic address");
                        int want = a.Op switch { IrAtomicOp.Load => 0, IrAtomicOp.Cas => 2, _ => 1 };
                        int have = (a.A != null ? 1 : 0) + (a.B != null ? 1 : 0);
                        if (have != want || (a.A == null && a.B != null)) Err($"atomic {a.Op} with {have} operands");
                        if (a.A != null) Want(a.A, a.Type, idom, "atomic operand");
                        if (a.B != null) Want(a.B, a.Type, idom, "atomic operand");
                        if ((a.Dst == null) != (a.Op == IrAtomicOp.Store)) Err($"atomic {a.Op} result");
                        else if (a.Dst != null && a.Dst.Type != a.Type) Err("atomic result must be its type");
                        break;
                    }
                default: Err("unknown instruction"); break;
            }
        }

        private void CheckCall(IrCall c, int[] idom)
        {
            Want(c.Callee, IrType.Ptr, idom, "callee");
            foreach (var a in c.Args)
            {
                var t = Use(a.Value, idom);
                if (a.AggSize > 0 && t != IrType.Ptr) Err("an agg argument must be an address");
                if (a.AggSize == 0 && t == IrType.Void) Err("a void argument");
            }
            if (c.AggDst != null) { Want(c.AggDst, IrType.Ptr, idom, "agg result address"); if (c.AggSize <= 0) Err("agg result of no size"); }
            if (c.AggDst != null && c.Dst != null) Err("a call with both a register and an agg result");

            if (c.Callee is not IrSym s || !_fns.TryGetValue(s.Name, out var fn)) return;
            if (fn.Params.Count != c.Args.Count) { Err($"@{s.Name} takes {fn.Params.Count} args, given {c.Args.Count}"); return; }
            for (int k = 0; k < fn.Params.Count; k++)
            {
                var p = fn.Params[k]; var a = c.Args[k];
                if (p.AggSize != a.AggSize) Err($"@{s.Name} arg {k + 1}: agg {a.AggSize}, wants agg {p.AggSize}");
                else if (p.AggSize == 0 && IrPrinter.TypeOf(a.Value) != p.Type)
                    Err($"@{s.Name} arg {k + 1}: {IrTypes.Name(IrPrinter.TypeOf(a.Value))}, wants {IrTypes.Name(p.Type)}");
            }
            var r = fn.Return;
            if (r.AggSize > 0) { if (c.AggDst == null || c.AggSize != r.AggSize) Err($"@{s.Name} returns agg {r.AggSize}"); }
            else if (r.Type == IrType.Void) { if (c.Dst != null || c.AggDst != null) Err($"@{s.Name} returns nothing"); }
            else if (c.Dst == null || c.Dst.Type != r.Type) Err($"@{s.Name} returns {IrTypes.Name(r.Type)}");
        }

        private void Target(int b)
        {
            if (b < 0 || b >= _f.Blocks.Count) Err($"jump to missing b{b}");
        }

        private void CheckTerm(IrTerm t, int[] idom)
        {
            switch (t)
            {
                case IrJump j: Target(j.Target); break;
                case IrBranch b: Want(b.Cond, IrType.Bool, idom, "branch condition"); Target(b.Then); Target(b.Else); break;
                case IrSwitch s:
                    {
                        var vt = Use(s.Value, idom);
                        if (!IrTypes.IsInt(vt)) Err($"switch on {IrTypes.Name(vt)}");
                        Target(s.Default);
                        var seen = new HashSet<UInt128>();
                        foreach (var (v, tgt) in s.Cases)
                        {
                            Target(tgt);
                            if (!seen.Add(v)) Err($"switch case {v} twice");
                            if (IrTypes.IsInt(vt) && (v & ~IrTypes.Mask(Bits(vt))) != 0) Err($"switch case {v} does not fit {IrTypes.Name(vt)}");
                        }
                        break;
                    }
                case IrRet r:
                    if (_f.Return.AggSize > 0) Err("ret in a function returning agg");
                    else if (_f.Return.Type == IrType.Void) { if (r.Value != null) Err("ret with a value in a void function"); }
                    else if (r.Value == null) Err("ret without a value");
                    else Want(r.Value, _f.Return.Type, idom, "ret value");
                    break;
                case IrRetAgg r:
                    if (_f.Return.AggSize != r.Size) Err($"ret agg {r.Size}, the function returns agg {_f.Return.AggSize}");
                    Want(r.Src, IrType.Ptr, idom, "ret agg source");
                    break;
                default: Err("unknown terminator"); break;
            }
        }
    }
}
