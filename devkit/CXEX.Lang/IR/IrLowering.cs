// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System;
using System.Collections.Generic;
using System.Linq;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;
using CXEX.Lang.Sema;

namespace CXEX.Lang.IR;

/// <summary>
/// Lower a checked compilation unit to the typed IR (Ir.cs, IR.md).
///
/// <para>The meaning is the x86-32 emitter's, decision for decision, so the two
/// can be held to each other: integers up to 32 bits are worked on at 32 and
/// cut back to their type, a wider operation takes the wider operand's type,
/// divide and right shift take the left operand's sign, and operands run in
/// the emitter's order (right before left, arguments last to first). Where
/// the emitter is wrong rather than merely particular, this says so at the
/// place and does the sane thing instead.</para>
/// </summary>
public sealed class IrLowering
{
    private readonly SemaContext _ctx;
    private readonly IReadOnlyDictionary<LetStmt, TypeRef> _localTypes;
    private readonly DiagnosticBag _diag;
    private readonly IrModule _m = new();
    private readonly Dictionary<string, string> _strings = new();

    private static readonly PrimType I32T = new(PrimKind.I32);

    public IrLowering(SemaContext ctx, IReadOnlyDictionary<LetStmt, TypeRef> localTypes, DiagnosticBag diag)
    {
        _ctx = ctx; _localTypes = localTypes; _diag = diag;
        _m.PointerBytes = ctx.Target.PointerBytes;
        _ctx.ExpandEnums = true;   // a plain enum is its backing integer from here on (SemaContext.ExpandEnums)
    }

    public IrModule Lower(CompilationUnit unit)
    {
        foreach (var d in unit.Decls)
        {
            if (d is FnDecl f) _m.Functions.Add(f.Body != null ? LowerFn(f) : Extern(f));
            else if (d is GlobalDecl g) _m.Globals.Add(LowerGlobal(g));
        }
        return _m;
    }

    // ---- types and layout (SemaContext's: the target's pointer width and alignment) ----
    private TypeRef X(TypeRef t) => _ctx.Expand(t);
    private StructDecl? StructOf(TypeRef t0) => X(t0) is NamedType n && _ctx.Structs.TryGetValue(n.Name, out var s) ? s : null;
    /* Bytes in memory, never a value: a struct, a sum type, an array. */
    private bool IsAgg(TypeRef t) => StructOf(t) != null || X(t) is ArrayType;
    /* An integer past 32 bits: worked on at its own width, not promoted. */
    private bool IsWide(TypeRef t) => X(t) is PrimType p && PrimWidth.Bytes(p.Kind, _m.PointerBytes) > 4;
    private bool Signed(TypeRef t) => X(t) is PrimType p && PrimWidth.IsSigned(p.Kind);
    private TypeRef TypeOf(Expr e) => _ctx.Types.TryGetValue(e, out var t) ? t : I32T;

    private int SizeOf(TypeRef t) => _ctx.SizeOf(t);
    private int StructSize(StructDecl s) => _ctx.StructSize(s.Fields);
    private int FieldOffset(StructDecl s, string field) => _ctx.FieldOffset(s, field);
    /* Where a sum type's payload starts, 4 bytes in on x86-32. */
    private int PayloadOffset(TypeRef sum) => X(sum) is NamedType n && _ctx.Enums.TryGetValue(n.Name, out var e) ? _ctx.PayloadOffset(e) : 4;

    private IrType Scalar(TypeRef t0) => X(t0) switch
    {
        PrimType { Kind: PrimKind.Bool } => IrType.Bool,
        PrimType { Kind: PrimKind.Void } => IrType.Void,
        PrimType p => IrTypes.IntOfBytes(PrimWidth.Bytes(p.Kind, _m.PointerBytes)),
        PointerType or FuncType => IrType.Ptr,
        _ => IrType.Void,
    };

    /* An address has no sign; neither does bool. */
    private bool SignedScalar(TypeRef t) => Signed(t);

    // ---- function state ----
    private IrFunction _fn = null!;
    private IrBlock _cur = null!;
    private int _nextReg, _nextBlock;
    private readonly Dictionary<Symbol, (IrValue Addr, TypeRef Type)> _vars = new();
    private TypeRef _curReturn = new PrimType(PrimKind.Void);
    private IrSlot? _retSlot;
    /* Defers and loops exactly as the emitter keeps them: one list per open
       block, and for each loop how many lists were open when it began. */
    private readonly List<List<Stmt>> _defers = new();
    private readonly Stack<(IrBlock Top, IrBlock End, int Depth)> _loops = new();

    private IrReg NewReg(IrType t) => new(_nextReg++, t);
    private IrBlock NewBlock() => new() { Id = _nextBlock++ };
    private IrSlotRef NewSlot(TypeRef t, string? name = null)
    {
        var s = new IrSlot { Id = _fn.Slots.Count, Size = Math.Max(SizeOf(t), 1), Align = _ctx.AlignOf(t), Name = name };
        _fn.Slots.Add(s);
        return new IrSlotRef(s.Id);
    }

    /* Code after a jump or a return is unreachable but still lowered (it may
       declare what later code names); it goes in a block of its own, which
       pruning then drops. */
    private void EnsureOpen()
    {
        if (_cur.Term == null) return;
        _cur = NewBlock();
        _fn.Blocks.Add(_cur);
    }
    private void Emit(IrInstr i) { EnsureOpen(); _cur.Instrs.Add(i); }
    private void Terminate(IrTerm t) { EnsureOpen(); _cur.Term = t; }
    private void JumpTo(IrBlock b) { if (_cur.Term == null) _cur.Term = new IrJump(b.Id); }
    /* Blocks join the function when they start, so the dump reads in source order. */
    private void StartBlock(IrBlock b) { JumpTo(b); _fn.Blocks.Add(b); _cur = b; }

    private IrValue Def(IrType t, Func<IrReg, IrInstr> make) { var r = NewReg(t); Emit(make(r)); return r; }
    private IrConst K(IrType t, UInt128 v) => new(t, v & IrTypes.Mask(IrTypes.Bits(t, _m.PointerBytes)));
    private IrValue Load(IrType t, IrValue addr) => Def(t, r => new IrLoad(r, addr));
    private IrValue PtrAdd(IrValue b, int off) => off == 0 ? b : Def(IrType.Ptr, r => new IrPtrAdd(r, b, K(_m.PtrInt, (UInt128)off)));

    // ---- declarations ----
    private IrFunction Extern(FnDecl f)
    {
        var fn = new IrFunction { Name = f.Name, IsExtern = true, Section = SectionOf(f) };
        foreach (var p in f.Params) fn.Params.Add(ParamOf(p.Name, p.Type));
        fn.Return = ParamOf("", f.Return);
        return fn;
    }

    private IrParam ParamOf(string name, TypeRef t) =>
        IsAgg(t) ? new IrParam(name, IrType.Void, SizeOf(t)) : new IrParam(name, Scalar(t));

    private IrFunction LowerFn(FnDecl f)
    {
        _fn = new IrFunction { Name = f.Name, Section = SectionOf(f) };
        _nextReg = 0; _nextBlock = 0;
        _vars.Clear(); _defers.Clear(); _loops.Clear();
        _curReturn = f.Return;
        _retSlot = null;
        _fn.Return = ParamOf("", f.Return);

        _cur = NewBlock();
        _fn.Blocks.Add(_cur);

        /* A scalar parameter arrives in a register and lives in a slot, so it
           can be assigned and have its address taken. An aggregate one
           arrives as the address of the function's own copy, which is its
           storage already. */
        var pending = new List<(IrReg Reg, IrSlotRef Slot)>();
        foreach (var p in f.Params)
        {
            var ip = ParamOf(p.Name, p.Type);
            _fn.Params.Add(ip);
            var reg = NewReg(ip.AggSize > 0 ? IrType.Ptr : ip.Type);
            _fn.ParamRegs.Add(reg);
            IrValue home = reg;
            if (ip.AggSize == 0) { var s = NewSlot(p.Type, p.Name); pending.Add((reg, s)); home = s; }
            if (_ctx.ParamSymbols.TryGetValue(p, out var ps)) _vars[ps] = (home, p.Type);
        }
        foreach (var (reg, slot) in pending) Emit(new IrStore(reg, slot));

        foreach (var l in CollectLocals(f.Body!))
        {
            var ty = _localTypes.TryGetValue(l, out var t) ? t : I32T;
            var s = NewSlot(ty, l.Name);
            if (_ctx.LetSymbols.TryGetValue(l, out var ls)) _vars[ls] = (s, ty);
        }

        LowerBlock(f.Body!);

        /* Falling off the end. The emitter returns whatever eax holds; zero
           here, so a back end has nothing undefined to reproduce. */
        if (_cur.Term == null)
        {
            if (_fn.Return.AggSize > 0) _cur.Term = new IrRetAgg(RetSlot(), _fn.Return.AggSize);
            else _cur.Term = new IrRet(_fn.Return.Type == IrType.Void ? null : K(_fn.Return.Type, 0));
        }

        PruneAndNumber(_fn);
        return _fn;
    }

    private IrSlotRef RetSlot()
    {
        if (_retSlot == null) { var r = NewSlot(_curReturn); _retSlot = _fn.Slots[r.Id]; }
        return new IrSlotRef(_retSlot.Id);
    }

    /* Drop the blocks nothing reaches and number the rest in order. */
    private static void PruneAndNumber(IrFunction fn)
    {
        var byId = fn.Blocks.ToDictionary(b => b.Id);
        var seen = new HashSet<int>();
        var work = new Stack<int>();
        work.Push(fn.Blocks[0].Id);
        while (work.Count > 0)
        {
            int id = work.Pop();
            if (!seen.Add(id)) continue;
            foreach (int s in Successors(byId[id].Term!)) work.Push(s);
        }
        fn.Blocks.RemoveAll(b => !seen.Contains(b.Id));
        var map = new Dictionary<int, int>();
        for (int i = 0; i < fn.Blocks.Count; i++) map[fn.Blocks[i].Id] = i;
        foreach (var b in fn.Blocks)
        {
            b.Id = map[b.Id];
            b.Term = b.Term switch
            {
                IrJump j => new IrJump(map[j.Target]),
                IrBranch br => br with { Then = map[br.Then], Else = map[br.Else] },
                IrSwitch sw => new IrSwitch(sw.Value, map[sw.Default], sw.Cases.Select(c => (c.Value, map[c.Target])).ToList()),
                var t => t,
            };
        }
    }

    public static IEnumerable<int> Successors(IrTerm t)
    {
        switch (t)
        {
            case IrJump j: yield return j.Target; break;
            case IrBranch b: yield return b.Then; yield return b.Else; break;
            case IrSwitch s: yield return s.Default; foreach (var c in s.Cases) yield return c.Target; break;
        }
    }

    private static string? SectionOf(Decl d)
    {
        foreach (var a in d.Attrs)
            if (a.Name == "section" && a.Args.Count == 1 && a.Args[0].Value is StrLit s) return s.Value;
        return null;
    }

    private IEnumerable<LetStmt> CollectLocals(Block b)
    {
        foreach (var s in b.Stmts)
            foreach (var l in LocalsIn(s)) yield return l;
    }
    private IEnumerable<LetStmt> LocalsIn(Stmt s)
    {
        switch (s)
        {
            case LetStmt l: yield return l; break;
            case Block b: foreach (var x in CollectLocals(b)) yield return x; break;
            case IfStmt i:
                foreach (var x in CollectLocals(i.Then)) yield return x;
                if (i.Else != null) foreach (var x in CollectLocals(i.Else)) yield return x; break;
            case WhileStmt w: foreach (var x in CollectLocals(w.Body)) yield return x; break;
            case DeferStmt d: foreach (var x in LocalsIn(d.Body)) yield return x; break;
            case SwitchStmt sw:
                foreach (var c in sw.Cases)
                {
                    if (c.BindLet != null) yield return c.BindLet;
                    foreach (var x in CollectLocals(c.Body)) yield return x;
                }
                if (sw.Else != null) foreach (var x in CollectLocals(sw.Else)) yield return x; break;
        }
    }

    /* A global: zero (Init null), or its bytes. A literal, a string or a
       function name is laid out piece by piece; anything else folds to a
       number written over the global's size, low byte first. */
    private IrGlobal LowerGlobal(GlobalDecl g)
    {
        var gl = new IrGlobal { Name = g.Name, Size = SizeOf(g.Type), Align = _ctx.AlignOf(g.Type), Section = SectionOf(g) };
        if (g.Init is StructLit or ArrayLit or StrLit || IsFunctionName(g.Init))
        {
            gl.Init = new List<IrData>();
            Data(gl.Init, g.Type, g.Init!);
            return gl;
        }
        UInt128 v = UInt128.Zero;
        if (g.Init != null) new ConstFold(_ctx, _diag).TryEval(g.Init, out v);
        if (v != UInt128.Zero) gl.Init = new List<IrData> { new IrDataBytes(LittleEndian(v, gl.Size)) };
        return gl;
    }

    private static byte[] LittleEndian(UInt128 v, int n)
    {
        var b = new byte[n];
        for (int i = 0; i < n && i < 16; i++) b[i] = (byte)(v >> (i * 8));
        return b;
    }

    private bool IsFunctionName(Expr? e) =>
        e is NameExpr n && _ctx.Resolved.TryGetValue(n, out var s) && s.Kind == SymKind.Function;

    /* Exactly SizeOf(t) bytes for `e`: the emitter's EmitData. */
    private void Data(List<IrData> o, TypeRef t, Expr e)
    {
        var x = X(t);
        if (e is StructLit sl && StructOf(x) is StructDecl sd)
        {
            int pos = 0;
            foreach (var f in sd.Fields)
            {
                int off = FieldOffset(sd, f.Name);
                if (off > pos) o.Add(new IrDataZero(off - pos));
                var fi = sl.Fields.Find(v => v.Name == f.Name);
                if (fi != null) Data(o, f.Type, fi.Value); else o.Add(new IrDataZero(SizeOf(f.Type)));
                pos = off + SizeOf(f.Type);
            }
            if (StructSize(sd) > pos) o.Add(new IrDataZero(StructSize(sd) - pos));
            return;
        }
        if (e is ArrayLit al && x is ArrayType at)
        {
            foreach (var i in al.Items) Data(o, at.Element, i);
            int missing = at.Length - al.Items.Count;
            if (missing > 0) o.Add(new IrDataZero(missing * SizeOf(at.Element)));
            return;
        }
        if (e is StrLit s) { o.Add(new IrDataAddr(InternString(s.Value))); return; }
        if (IsFunctionName(e)) { o.Add(new IrDataAddr(((NameExpr)e).Name)); return; }
        new ConstFold(_ctx, _diag).TryEval(e, out var v);
        o.Add(new IrDataBytes(LittleEndian(v, SizeOf(x))));
    }

    private string InternString(string s)
    {
        if (_strings.TryGetValue(s, out var name)) return name;
        name = "str." + _m.Strings.Count;
        var bytes = System.Text.Encoding.UTF8.GetBytes(s);
        Array.Resize(ref bytes, bytes.Length + 1);   // NUL
        _m.Strings.Add(new IrString { Name = name, Bytes = bytes });
        _strings[s] = name;
        return name;
    }

    // ---- statements ----
    private void LowerBlock(Block b)
    {
        _defers.Add(new List<Stmt>());
        foreach (var s in b.Stmts) LowerStmt(s);
        RunDefers(_defers.Count - 1);
        _defers.RemoveAt(_defers.Count - 1);
    }

    /* Innermost block first, last-deferred first; each exit gets its own copy. */
    private void RunDefers(int downTo)
    {
        for (int i = _defers.Count - 1; i >= downTo; i--)
        {
            var pending = _defers[i].ToArray();
            for (int j = pending.Length - 1; j >= 0; j--) LowerStmt(pending[j]);
        }
    }

    private void LowerStmt(Stmt s)
    {
        switch (s)
        {
            case Block b: LowerBlock(b); break;
            case LetStmt l:
                if (l.Init != null && _ctx.LetSymbols.TryGetValue(l, out var ls) && _vars.TryGetValue(ls, out var v))
                    StoreAt(() => v.Addr, v.Type, l.Init);
                break;
            case AssignStmt a: LowerAssign(a); break;
            case ExprStmt e: RValue(e.Expr); break;
            case ReturnStmt r: LowerReturn(r); break;
            case DeferStmt d: if (_defers.Count > 0) _defers[^1].Add(d.Body); break;
            case SwitchStmt sw: LowerSwitch(sw); break;
            case IfStmt i:
                {
                    var then = NewBlock(); var end = NewBlock();
                    var els = i.Else != null ? NewBlock() : end;
                    Terminate(new IrBranch(Cond(i.Cond), then.Id, els.Id));
                    StartBlock(then); LowerBlock(i.Then); JumpTo(end);
                    if (i.Else != null) { StartBlock(els); LowerBlock(i.Else); }
                    StartBlock(end);
                    break;
                }
            case WhileStmt w:
                {
                    var top = NewBlock(); var body = NewBlock(); var end = NewBlock();
                    StartBlock(top);
                    Terminate(new IrBranch(Cond(w.Cond), body.Id, end.Id));
                    _loops.Push((top, end, _defers.Count));
                    StartBlock(body); LowerBlock(w.Body);
                    _loops.Pop();
                    JumpTo(top);
                    StartBlock(end);
                    break;
                }
            case BreakStmt:
                if (_loops.Count > 0) { RunDefers(_loops.Peek().Depth); Terminate(new IrJump(_loops.Peek().End.Id)); }
                break;
            case ContinueStmt:
                if (_loops.Count > 0) { RunDefers(_loops.Peek().Depth); Terminate(new IrJump(_loops.Peek().Top.Id)); }
                break;
        }
    }

    /* Store `e` as a `t` at the address `addr` yields. The value is computed
       first, as the emitter does: an assignment's right side runs before its
       target's address. */
    private void StoreAt(Func<IrValue> addr, TypeRef t, Expr e)
    {
        if (IsAgg(t)) { var src = RValue(e)!; Emit(new IrCopy(addr(), src, SizeOf(t))); return; }
        var v = Convert(RValue(e), TypeOf(e), t, e.Span);
        Emit(new IrStore(v, addr()));
    }

    private void LowerAssign(AssignStmt a) => StoreAt(() => Addr(a.Target), TypeOf(a.Target), a.Value);

    /* The value is taken before the defers run - `return x` returns x as it
       was - so an aggregate is copied out to the return slot first. */
    private void LowerReturn(ReturnStmt r)
    {
        if (_fn.Return.AggSize > 0)
        {
            var slot = RetSlot();
            if (r.Value != null) Emit(new IrCopy(slot, RValue(r.Value)!, _fn.Return.AggSize));
            RunDefers(0);
            Terminate(new IrRetAgg(slot, _fn.Return.AggSize));
            return;
        }
        IrValue? v = null;
        if (r.Value != null && _fn.Return.Type != IrType.Void) v = Convert(RValue(r.Value), TypeOf(r.Value), _curReturn, r.Span);
        else if (r.Value != null) RValue(r.Value);
        else if (_fn.Return.Type != IrType.Void) v = K(_fn.Return.Type, 0);
        RunDefers(0);
        Terminate(new IrRet(v));
    }

    /* The subject is evaluated once: a value, or for a sum type its address,
       whose tag picks the case and whose payload (at PayloadOffset: 4 bytes
       in on x86-32) a binding points at. No fallthrough. */
    private void LowerSwitch(SwitchStmt sw)
    {
        var st = TypeOf(sw.Subject);
        bool sum = X(st) is NamedType sn && _ctx.Enums.TryGetValue(sn.Name, out var se) && se.IsSum;
        var subj = RValue(sw.Subject) ?? K(IrType.I32, 0);
        IrValue key = subj;
        IrType kt = sum ? IrType.I32 : Scalar(st);
        if (sum) key = Load(IrType.I32, subj);
        else if (kt == IrType.Bool) { key = Conv(subj, IrType.Bool, false, IrType.I32); kt = IrType.I32; }
        else if (!IrTypes.IsInt(kt)) { _diag.Error($"cannot switch on {kt}", sw.Span); key = K(IrType.I32, 0); kt = IrType.I32; }

        var end = NewBlock();
        var els = sw.Else != null ? NewBlock() : end;
        var bodies = new List<IrBlock>();
        var cases = new List<(UInt128, int)>();
        var fold = new ConstFold(_ctx, _diag);
        foreach (var c in sw.Cases)
        {
            var body = NewBlock();
            bodies.Add(body);
            foreach (var l in c.Labels)
            {
                UInt128 v;
                if (l is MemberExpr lm && _ctx.EnumRefs.TryGetValue(lm, out var er))
                {
                    if (sum) v = (UInt128)er.Index; else fold.TryEnumValue(er.Enum, er.Index, out v);
                }
                else if (!fold.TryEval(l, out v)) continue;
                v &= IrTypes.Mask(IrTypes.Bits(kt, _m.PointerBytes));
                if (!cases.Exists(x => x.Item1 == v)) cases.Add((v, body.Id));   // the first label wins, as in a compare chain
            }
        }
        Terminate(new IrSwitch(key, els.Id, cases));

        for (int i = 0; i < sw.Cases.Count; i++)
        {
            var c = sw.Cases[i];
            StartBlock(bodies[i]);
            if (c.BindLet != null && _ctx.LetSymbols.TryGetValue(c.BindLet, out var bs) && _vars.TryGetValue(bs, out var b))
                Emit(new IrStore(PtrAdd(subj, PayloadOffset(st)), b.Addr));
            LowerBlock(c.Body);
            JumpTo(end);
        }
        if (sw.Else != null) { StartBlock(els); LowerBlock(sw.Else); }
        StartBlock(end);
    }

    private IrValue Cond(Expr e)
    {
        var v = RValue(e) ?? K(IrType.Bool, 0);
        var t = Scalar(TypeOf(e));
        return t == IrType.Bool ? v : Conv(v, t, false, IrType.Bool);
    }

    // ---- conversions ----
    private IrValue Convert(IrValue? v, TypeRef from, TypeRef to, SourceSpan span)
    {
        if (v == null) { _diag.Error("a void value is used", span); return K(IrType.I32, 0); }
        if (IsAgg(to) || IsAgg(from)) return v;
        var tt = Scalar(to);
        if (tt == IrType.Void) return v;
        return Conv(v, Scalar(from), SignedScalar(from), tt);
    }

    /* Between two value types: an integer narrows by truncation and widens by
       its own sign (C's rule, and the emitter's); a pointer goes through the
       pointer-width integer; bool widens by zero, and becomes bool by
       comparing with zero. A constant is converted here rather than with an
       instruction. */
    private IrValue Conv(IrValue v, IrType ft, bool fsigned, IrType tt)
    {
        if (ft == tt) return v;
        if (ft == IrType.Ptr)
        {
            var i = v is IrConst c0 ? K(_m.PtrInt, c0.Value) : Def(_m.PtrInt, r => new IrConvert(r, IrConv.PtrToInt, v));
            return Conv(i, _m.PtrInt, false, tt);
        }
        if (tt == IrType.Ptr)
        {
            var i = Conv(v, ft, fsigned, _m.PtrInt);
            return Def(IrType.Ptr, r => new IrConvert(r, IrConv.IntToPtr, i));
        }
        if (tt == IrType.Bool)
        {
            if (v is IrConst cb) return K(IrType.Bool, cb.Value != 0 ? UInt128.One : UInt128.Zero);
            return Def(IrType.Bool, r => new IrCompare(r, IrCmp.Ne, v, K(ft, 0)));
        }
        if (ft == IrType.Void || tt == IrType.Void) return v;
        int fb = IrTypes.Bits(ft, _m.PointerBytes), tb = IrTypes.Bits(tt, _m.PointerBytes);
        bool sext = fsigned && ft != IrType.Bool;
        if (v is IrConst c)
        {
            UInt128 x = c.Value;
            if (tb > fb && sext && ((x >> (fb - 1)) & 1) != 0) x |= IrTypes.Mask(tb) & ~IrTypes.Mask(fb);
            return K(tt, x);
        }
        var kind = tb < fb ? IrConv.Trunc : sext ? IrConv.SExt : IrConv.ZExt;
        return Def(tt, r => new IrConvert(r, kind, v));
    }

    // ---- expressions: a value, or for an aggregate the address of its bytes; null for void ----
    private IrValue? RValue(Expr e)
    {
        switch (e)
        {
            case IntLit i: return K(Scalar(TypeOf(i)), i.Value);
            case StrLit s: return new IrSym(InternString(s.Value));
            case SizeofExpr sz: return K(Scalar(TypeOf(sz)), (UInt128)SizeOf(TypeOf(sz.Operand)));   // not evaluated
            case BoolLit b: return K(IrType.Bool, b.Value ? UInt128.One : UInt128.Zero);
            case NameExpr n: return LowerName(n);
            case BinaryExpr b: return LowerBinary(b);
            case UnaryExpr u: return LowerUnary(u);
            case CallExpr c: return LowerCall(c);
            case CastExpr c: return LowerCast(c);
            case StructLit sl: return LowerStructLit(sl);
            case ArrayLit al: return LowerArrayLit(al);
            case MemberExpr em when _ctx.EnumRefs.TryGetValue(em, out var er): return LowerVariant(er.Enum, er.Index, TypeOf(em));
            case MemberExpr or IndexExpr:
                {
                    var t = TypeOf(e);
                    var a = Addr(e);
                    return IsAgg(t) ? a : Load(Scalar(t), a);
                }
            default:
                _diag.Error("this expression cannot be lowered", e.Span);
                return K(IrType.I32, 0);
        }
    }

    private IrValue LowerName(NameExpr n)
    {
        if (!_ctx.Resolved.TryGetValue(n, out var sym)) { _diag.Error($"'{n.Name}' is not resolved", n.Span); return K(IrType.I32, 0); }
        if (sym.Kind == SymKind.Const && sym.Decl is ConstDecl cd && new ConstFold(_ctx, _diag).TryEval(cd.Value, out var v))
            return K(Scalar(cd.Type), v);   // cut to the constant's own type
        if (sym.Kind == SymKind.Function) return new IrSym(n.Name);
        if (sym.Kind == SymKind.Global)
        {
            var gt = sym.Decl is GlobalDecl gd ? gd.Type : sym.Type;
            return IsAgg(gt) ? new IrSym(n.Name) : Load(Scalar(gt), new IrSym(n.Name));
        }
        if (_vars.TryGetValue(sym, out var var)) return IsAgg(var.Type) ? var.Addr : Load(Scalar(var.Type), var.Addr);
        _diag.Error($"'{n.Name}' has no storage", n.Span);
        return K(IrType.I32, 0);
    }

    /* The address of an lvalue. */
    private IrValue Addr(Expr e)
    {
        switch (e)
        {
            case NameExpr n when _ctx.Resolved.TryGetValue(n, out var sym):
                if (_vars.TryGetValue(sym, out var v)) return v.Addr;
                if (sym.Kind == SymKind.Global) return new IrSym(n.Name);
                break;
            case UnaryExpr { Op: UnOp.Deref } u: return RValue(u.Operand)!;
            case MemberExpr m when !_ctx.EnumRefs.ContainsKey(m):
                {
                    var tt = TypeOf(m.Target);
                    bool viaPtr = X(tt) is PointerType;
                    var sd = StructOf(viaPtr ? ((PointerType)X(tt)).Pointee : tt);
                    var b = viaPtr ? RValue(m.Target)! : Addr(m.Target);   // ps.f: the pointer's value; s.f: s's address
                    return sd != null ? PtrAdd(b, FieldOffset(sd, m.Field)) : b;
                }
            case IndexExpr ix:
                {
                    var te = X(TypeOf(ix.Target));
                    int es = te switch { PointerType p => SizeOf(p.Pointee), ArrayType a => SizeOf(a.Element), _ => 4 };
                    var b = te is ArrayType ? Addr(ix.Target) : RValue(ix.Target)!;
                    var it = TypeOf(ix.Index);
                    var i = Conv(RValue(ix.Index) ?? K(IrType.I32, 0), Scalar(it), Signed(it), _m.PtrInt);
                    IrValue off = es == 1 ? i
                        : i is IrConst ci ? K(_m.PtrInt, ci.Value * (UInt128)es)
                        : Def(_m.PtrInt, r => new IrBinary(r, IrBinOp.Mul, i, K(_m.PtrInt, (UInt128)es)));
                    if (off is IrConst { Value: var z } && z == UInt128.Zero) return b;
                    return Def(IrType.Ptr, r => new IrPtrAdd(r, b, off));
                }
        }
        /* A value that is its address - a call returning a struct, a literal -
           is its own address, so `make().y` reads the call's result. */
        if (IsAgg(TypeOf(e))) return RValue(e)!;
        _diag.Error("this expression has no address", e.Span);
        return K(IrType.Ptr, 0);
    }

    private static bool IsCompare(BinOp op) =>
        op is BinOp.Eq or BinOp.Ne or BinOp.Lt or BinOp.Le or BinOp.Gt or BinOp.Ge;

    private IrValue LowerBinary(BinaryExpr b)
    {
        if (b.Op is BinOp.And or BinOp.Or) return LowerShortCircuit(b);

        TypeRef lt = TypeOf(b.Left), rt = TypeOf(b.Right), res = TypeOf(b);
        bool cmp = IsCompare(b.Op), shift = b.Op is BinOp.Shl or BinOp.Shr;

        if (cmp && (IsAgg(lt) || IsAgg(rt)))
        {
            _diag.Error($"{b.Op} on an aggregate cannot be lowered", b.Span);
            return K(IrType.Bool, 0);
        }

        /* Past 32 bits: both operands at the wider type (a shift's count at
           the left's), which also fixes the sign; the right one first. */
        if (IsWide(res) || (cmp && (IsWide(lt) || IsWide(rt))))
        {
            var wt = lt;
            if (!shift && SizeOf(rt) > SizeOf(lt)) wt = rt;
            var t = Scalar(wt);
            bool s = Signed(wt);
            var r = Convert(RValue(b.Right), rt, wt, b.Span);
            var l = Convert(RValue(b.Left), lt, wt, b.Span);
            if (cmp) return Def(IrType.Bool, d => new IrCompare(d, CmpOf(b.Op, s), l, r));
            var v = Def(t, d => new IrBinary(d, BinOf(b.Op, s), l, r));
            return Conv(v, t, s, Scalar(res));
        }

        var rv = RValue(b.Right) ?? K(IrType.I32, 0);
        var lv = RValue(b.Left) ?? K(IrType.I32, 0);
        IrType ls = Scalar(lt), rs = Scalar(rt);

        if (cmp)
        {
            /* Same type: compare at it. Otherwise C's rule, the emitter's: both
               widen to 32 bits by their own sign, and the comparison is
               unsigned if either is an unsigned 32-bit value or an address. */
            if (ls == rs && Signed(lt) == Signed(rt))
                return Def(IrType.Bool, d => new IrCompare(d, CmpOf(b.Op, Signed(lt)), lv, rv));
            bool signed = !(UnsignedWord(lt) || UnsignedWord(rt));
            var l32 = Conv(lv, ls, Signed(lt), IrType.I32);
            var r32 = Conv(rv, rs, Signed(rt), IrType.I32);
            return Def(IrType.Bool, d => new IrCompare(d, CmpOf(b.Op, signed), l32, r32));
        }

        var rt0 = Scalar(res);
        if (b.Op is BinOp.Add or BinOp.Sub or BinOp.Mul or BinOp.BitAnd or BinOp.BitOr or BinOp.BitXor)
        {
            /* The low bits of these depend only on the operands' low bits, so
               working at the result's width is the same as working at 32 and
               cutting back. */
            var l = Conv(lv, ls, Signed(lt), rt0);
            var r = Conv(rv, rs, Signed(rt), rt0);
            return Def(rt0, d => new IrBinary(d, BinOf(b.Op, false), l, r));
        }

        /* Divide, remainder and shifts at 32 bits, each operand widened by its
           own sign, the operation taking the LEFT operand's; then cut back.
           A shift count is reduced modulo 32, as the CPU does. */
        bool ls32 = Signed(lt);
        var a = Conv(lv, ls, ls32, IrType.I32);
        var c = Conv(rv, rs, Signed(rt), IrType.I32);
        var v32 = Def(IrType.I32, d => new IrBinary(d, BinOf(b.Op, ls32), a, c));
        return Conv(v32, IrType.I32, ls32, rt0);
    }

    private bool UnsignedWord(TypeRef t0)
    {
        var t = X(t0);
        if (t is PointerType or FuncType) return true;
        return t is PrimType p && !PrimWidth.IsSigned(p.Kind) && PrimWidth.Bytes(p.Kind, _m.PointerBytes) == 4;
    }

    private static IrBinOp BinOf(BinOp op, bool signed) => op switch
    {
        BinOp.Add => IrBinOp.Add,
        BinOp.Sub => IrBinOp.Sub,
        BinOp.Mul => IrBinOp.Mul,
        BinOp.Div => signed ? IrBinOp.SDiv : IrBinOp.UDiv,
        BinOp.Mod => signed ? IrBinOp.SRem : IrBinOp.URem,
        BinOp.BitAnd => IrBinOp.And,
        BinOp.BitOr => IrBinOp.Or,
        BinOp.BitXor => IrBinOp.Xor,
        BinOp.Shl => IrBinOp.Shl,
        BinOp.Shr => signed ? IrBinOp.AShr : IrBinOp.LShr,
        _ => throw new ArgumentOutOfRangeException(nameof(op)),
    };

    private static IrCmp CmpOf(BinOp op, bool signed) => op switch
    {
        BinOp.Eq => IrCmp.Eq,
        BinOp.Ne => IrCmp.Ne,
        BinOp.Lt => signed ? IrCmp.Slt : IrCmp.Ult,
        BinOp.Le => signed ? IrCmp.Sle : IrCmp.Ule,
        BinOp.Gt => signed ? IrCmp.Sgt : IrCmp.Ugt,
        BinOp.Ge => signed ? IrCmp.Sge : IrCmp.Uge,
        _ => throw new ArgumentOutOfRangeException(nameof(op)),
    };

    /* `a && b`, `a || b`: b runs only when it decides. The result goes
       through a slot, since a register is assigned once. */
    private IrValue LowerShortCircuit(BinaryExpr b)
    {
        var slot = NewSlot(new PrimType(PrimKind.Bool));
        var l = Cond(b.Left);
        Emit(new IrStore(l, slot));
        var rhs = NewBlock(); var end = NewBlock();
        Terminate(b.Op == BinOp.And ? new IrBranch(l, rhs.Id, end.Id) : new IrBranch(l, end.Id, rhs.Id));
        StartBlock(rhs);
        Emit(new IrStore(Cond(b.Right), slot));
        StartBlock(end);
        return Load(IrType.Bool, slot);
    }

    private IrValue? LowerUnary(UnaryExpr u)
    {
        var ut = TypeOf(u);
        switch (u.Op)
        {
            case UnOp.Neg:
            case UnOp.BitNot:
                {
                    var v = Convert(RValue(u.Operand), TypeOf(u.Operand), ut, u.Span);
                    if (v is IrConst c && u.Op == UnOp.Neg) return K(c.Type, UInt128.Zero - c.Value);
                    return Def(Scalar(ut), d => new IrUnary(d, u.Op == UnOp.Neg ? IrUnOp.Neg : IrUnOp.Not, v));
                }
            case UnOp.Not:
                {
                    var v = Cond(u.Operand);
                    return Def(IrType.Bool, d => new IrCompare(d, IrCmp.Eq, v, K(IrType.Bool, 0)));
                }
            case UnOp.Deref:
                {
                    var p = RValue(u.Operand) ?? K(IrType.Ptr, 0);
                    return IsAgg(ut) ? p : Load(Scalar(ut), p);
                }
            case UnOp.AddrOf: return Addr(u.Operand);
        }
        return null;
    }

    /* `x as T`. Between value types, Conv's rules. `as bool` compares with
       zero: the emitter left the whole register, so a bool could hold 300
       until it was stored and became 44 - not a meaning worth keeping. */
    private IrValue? LowerCast(CastExpr c)
    {
        var to = TypeOf(c); var from = TypeOf(c.Operand);
        var v = RValue(c.Operand);
        if (IsAgg(to) || IsAgg(from))
        {
            if (IsAgg(to) && IsAgg(from) && SizeOf(to) == SizeOf(from)) return v;
            _diag.Error("a cast to or from an aggregate cannot be lowered", c.Span);
            return v;
        }
        return Convert(v, from, to, c.Span);
    }

    private IrValue? LowerCall(CallExpr c)
    {
        if (c.Callee is NameExpr { Name: "__syscall" }) return LowerSyscall(c);
        var ptypes = ParamTypesOf(c);
        var rt = TypeOf(c);
        var args = new IrArg[c.Args.Count];
        for (int i = c.Args.Count - 1; i >= 0; i--)   // last to first, as cdecl pushes them
        {
            var a = c.Args[i];
            var pt = i < ptypes.Count ? ptypes[i] : TypeOf(a);
            args[i] = IsAgg(pt) ? new IrArg(RValue(a)!, SizeOf(pt)) : new IrArg(Convert(RValue(a), TypeOf(a), pt, a.Span));
        }
        /* Only a name that resolves to a function is a direct call; one
           holding a function pointer is called through its value, which is
           evaluated after the arguments. */
        IrValue callee = c.Callee is NameExpr nm && _ctx.Resolved.TryGetValue(nm, out var cs) && cs.Kind == SymKind.Function
            ? new IrSym(nm.Name)
            : RValue(c.Callee) ?? K(IrType.Ptr, 0);

        if (IsAgg(rt))
        {
            var slot = NewSlot(rt);
            Emit(new IrCall(null, callee, args.ToList(), slot, SizeOf(rt)));
            return slot;
        }
        var t = Scalar(rt);
        if (t == IrType.Void) { Emit(new IrCall(null, callee, args.ToList())); return null; }
        return Def(t, d => new IrCall(d, callee, args.ToList()));
    }

    private List<TypeRef> ParamTypesOf(CallExpr c)
    {
        if (c.Callee is NameExpr nm && _ctx.Resolved.TryGetValue(nm, out var sym) && sym.Decl is FnDecl fn)
            return fn.Params.Select(p => p.Type).ToList();
        if (X(TypeOf(c.Callee)) is FuncType ft) return ft.Params;
        return new List<TypeRef>();
    }

    /* __syscall(n, a1..a5): first to last, each a pointer-width word. A wide
       argument has no single word to be; the ABI splits one into a register
       pair (CX_SYSCALLS_IPC.md §9.5) and the program must do that itself. */
    private IrValue LowerSyscall(CallExpr c)
    {
        var words = new List<IrValue>();
        foreach (var a in c.Args)
        {
            var t = TypeOf(a);
            if (SizeOf(t) > _m.PointerBytes || IsAgg(t)) _diag.Error("a __syscall argument wider than a pointer cannot be passed in one register", a.Span);
            words.Add(Conv(RValue(a) ?? K(IrType.I32, 0), Scalar(t), Signed(t), _m.PtrInt));
        }
        return Def(Scalar(TypeOf(c)), d => new IrSyscall(d, words));
    }

    /* A record literal, built field by field in a slot of its own; the slot's
       address is the value. A sum variant's: the tag (its index), a zeroed
       payload so copies are deterministic, then the fields after the tag. */
    private IrValue LowerStructLit(StructLit sl)
    {
        var slot = NewSlot(TypeOf(sl));
        if (sl.TypeName is MemberExpr vm && _ctx.EnumRefs.TryGetValue(vm, out var er))
        {
            var en = er.Enum;
            int size = SizeOf(new NamedType(en.Name));
            int at = _ctx.PayloadOffset(en);
            Emit(new IrStore(K(IrType.I32, (UInt128)er.Index), slot));
            if (size > 4) Emit(new IrZero(PtrAdd(slot, 4), size - 4));
            if (_ctx.Structs.TryGetValue(SemaContext.PayloadName(en.Name, en.Variants[er.Index].Name), out var pd))
                foreach (var fi in sl.Fields)
                {
                    var decl = pd.Fields.Find(f => f.Name == fi.Name);
                    if (decl != null) StoreAt(() => PtrAdd(slot, at + FieldOffset(pd, fi.Name)), decl.Type, fi.Value);
                }
            return slot;
        }
        var sd = StructOf(TypeOf(sl));
        if (sd != null)
            foreach (var fi in sl.Fields)
            {
                var decl = sd.Fields.Find(f => f.Name == fi.Name);
                if (decl != null) StoreAt(() => PtrAdd(slot, FieldOffset(sd, fi.Name)), decl.Type, fi.Value);
            }
        return slot;
    }

    private IrValue LowerArrayLit(ArrayLit al)
    {
        var at = TypeOf(al);
        var slot = NewSlot(at);
        if (X(at) is ArrayType a)
        {
            int es = SizeOf(a.Element);
            for (int i = 0; i < al.Items.Count; i++)
            {
                int off = i * es;
                StoreAt(() => PtrAdd(slot, off), a.Element, al.Items[i]);
            }
        }
        return slot;
    }

    /* `color.red` is its value; a sum type's field-less variant is a whole sum
       value, built in a slot like a literal. */
    private IrValue LowerVariant(EnumDecl ed, int index, TypeRef t)
    {
        if (!ed.IsSum)
        {
            new ConstFold(_ctx, _diag).TryEnumValue(ed, index, out var v);
            return K(Scalar(t), v);
        }
        int size = SizeOf(new NamedType(ed.Name));
        var slot = NewSlot(new NamedType(ed.Name));
        Emit(new IrStore(K(IrType.I32, (UInt128)index), slot));
        if (size > 4) Emit(new IrZero(PtrAdd(slot, 4), size - 4));
        return slot;
    }
}
