// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using CXEX.Lang.IR;

namespace CXEX.Tests.Unit;

/// <summary>
/// Runs IR by the rules in <c>devkit/CXEX.Lang/IR/IR.md</c>, so the lowering's
/// MEANING is tested before any back end exists, on any host: a program from
/// <c>tests/lang/run</c> lowered and run here must still return 0. Flat
/// little-endian memory, the first page unmapped so a null access traps, the
/// stack at the top. No system calls and no extern functions: a program
/// that needs either is not one this can run.
/// </summary>
public sealed class IrInterpreter
{
    public sealed class Trap(string message) : Exception(message);

    private const uint MemSize = 16 << 20;
    private const uint FnBase = 0xF000_0000;   // functions get addresses out of memory's way
    private readonly byte[] _mem = new byte[MemSize];
    private readonly IrModule _m;
    private readonly Dictionary<string, uint> _addr = new();
    private readonly Dictionary<uint, IrFunction> _fnAt = new();
    private readonly Dictionary<string, IrFunction> _fns = new();
    private uint _sp = MemSize;
    private long _steps;
    public long StepLimit { get; init; } = 200_000_000;

    public IrInterpreter(IrModule m)
    {
        if (m.PointerBytes != 4) throw new ArgumentException("the interpreter models a 4-byte pointer");
        _m = m;
        uint at = 0x1000;
        for (int i = 0; i < m.Functions.Count; i++)
        {
            var f = m.Functions[i];
            uint a = FnBase + (uint)i * 16;
            _addr[f.Name] = a; _fnAt[a] = f; _fns[f.Name] = f;
        }
        foreach (var s in m.Strings)
        {
            _addr[s.Name] = at;
            s.Bytes.CopyTo(_mem, at);
            at = (at + (uint)s.Bytes.Length + 3) & ~3u;
        }
        foreach (var g in m.Globals) { _addr[g.Name] = at; at = (at + (uint)Math.Max(g.Size, 1) + 3) & ~3u; }
        foreach (var g in m.Globals)
        {
            uint p = _addr[g.Name];
            foreach (var d in g.Init ?? new List<IrData>())
                switch (d)
                {
                    case IrDataBytes b: b.Bytes.CopyTo(_mem, p); p += (uint)b.Bytes.Length; break;
                    case IrDataAddr a: Write(p, 4, _addr[a.Symbol]); p += 4; break;
                    case IrDataZero z: p += (uint)z.Size; break;
                }
        }
    }

    /// <summary>Call a function that takes scalar arguments and returns a scalar.</summary>
    public UInt128 Call(string name, params UInt128[] args) =>
        Run(_fns[name], args.Select(a => (a, 0)).ToList(), 0);

    private int Bits(IrType t) => IrTypes.Bits(t, _m.PointerBytes);
    private UInt128 Mask(IrType t, UInt128 v) => v & IrTypes.Mask(Bits(t));

    private void Check(uint a, int n)
    {
        if (a < 0x1000 || (ulong)a + (ulong)n > MemSize) throw new Trap($"access of {n} bytes at 0x{a:X8}");
    }
    private UInt128 Read(uint a, int n)
    {
        Check(a, n);
        UInt128 v = 0;
        for (int i = n - 1; i >= 0; i--) v = (v << 8) | _mem[a + i];
        return v;
    }
    private void Write(uint a, int n, UInt128 v)
    {
        Check(a, n);
        for (int i = 0; i < n; i++) _mem[a + i] = (byte)(v >> (i * 8));
    }

    private Int128 Signed(IrType t, UInt128 v)
    {
        int b = Bits(t);
        if (b < 128 && ((v >> (b - 1)) & 1) != 0) v |= ~IrTypes.Mask(b);
        return (Int128)v;
    }

    private UInt128 Run(IrFunction f, List<(UInt128 Value, int Agg)> args, uint aggDst)
    {
        if (f.IsExtern) throw new Trap($"call of extern @{f.Name}");
        uint savedSp = _sp;
        var slots = new uint[f.Slots.Count];
        foreach (var s in f.Slots)
        {
            _sp = (_sp - (uint)s.Size) & ~(uint)(s.Align - 1);
            slots[s.Id] = _sp;
            /* Not zero: the emitter's frames hold whatever was there, so a
               lowering that forgets to write a field must not pass by luck. */
            _mem.AsSpan((int)_sp, s.Size).Fill(0xCD);
        }
        var regs = new Dictionary<int, UInt128>();
        for (int i = 0; i < f.Params.Count; i++)
        {
            var (v, agg) = args[i];
            if (agg > 0)
            {
                _sp = (_sp - (uint)agg) & ~3u;
                Check(_sp, agg);
                Buffer.BlockCopy(_mem, (int)v, _mem, (int)_sp, agg);
                v = _sp;
            }
            regs[f.ParamRegs[i].Id] = v;
        }
        if (_sp < 0x100000) throw new Trap("stack overflow");

        UInt128 Val(IrValue x) => x switch
        {
            IrReg r => regs.TryGetValue(r.Id, out var v) ? v : throw new Trap($"@{f.Name}: %{r.Id} read before it is assigned"),
            IrConst c => c.Value,
            IrSym s => _addr[s.Name],
            IrSlotRef s => slots[s.Id],
            _ => throw new Trap("operand"),
        };
        uint P(IrValue x) => (uint)Val(x);

        var blk = f.Blocks[0];
        while (true)
        {
            foreach (var ins in blk.Instrs)
            {
                if (++_steps > StepLimit) throw new Trap("step limit");
                switch (ins)
                {
                    case IrBinary b: regs[b.Dst.Id] = Binary(b, Val(b.A), Val(b.B)); break;
                    case IrUnary u:
                        regs[u.Dst.Id] = Mask(u.Dst.Type, u.Op == IrUnOp.Neg ? UInt128.Zero - Val(u.A) : ~Val(u.A));
                        break;
                    case IrCompare c: regs[c.Dst.Id] = Compare(c, Val(c.A), Val(c.B)) ? 1u : 0u; break;
                    case IrConvert c:
                        {
                            var from = IrPrinter.TypeOf(c.A);
                            var v = Val(c.A);
                            regs[c.Dst.Id] = c.Kind == IrConv.SExt ? Mask(c.Dst.Type, (UInt128)Signed(from, v)) : Mask(c.Dst.Type, v);
                            break;
                        }
                    case IrLoad l: regs[l.Dst.Id] = Read(P(l.Addr), IrTypes.Bytes(l.Dst.Type, 4)); break;
                    case IrStore s: Write(P(s.Addr), IrTypes.Bytes(IrPrinter.TypeOf(s.Value), 4), Val(s.Value)); break;
                    case IrCopy c:
                        {
                            uint d = P(c.Dst), s = P(c.Src);
                            Check(d, c.Size); Check(s, c.Size);
                            Buffer.BlockCopy(_mem, (int)s, _mem, (int)d, c.Size);
                            break;
                        }
                    case IrZero z: Check(P(z.Dst), z.Size); Array.Clear(_mem, (int)P(z.Dst), z.Size); break;
                    case IrPtrAdd p: regs[p.Dst.Id] = (uint)(Val(p.Base) + Val(p.Offset)); break;
                    case IrCall c:
                        {
                            uint fa = P(c.Callee);
                            if (!_fnAt.TryGetValue(fa, out var callee)) throw new Trap($"call of 0x{fa:X8}, not a function");
                            var a = c.Args.Select(x => (Val(x.Value), x.AggSize)).ToList();
                            var r = Run(callee, a, c.AggDst != null ? P(c.AggDst) : 0);
                            if (c.Dst != null) regs[c.Dst.Id] = Mask(c.Dst.Type, r);
                            break;
                        }
                    case IrSyscall: throw new Trap("system call");
                    case IrTlsBase: throw new Trap("thread-local base");
                    case IrAtomic at:
                        {
                            /* One thread: atomic is ordinary, and the old value is the result. */
                            uint addr = P(at.Addr);
                            int n = IrTypes.Bytes(at.Type, 4);
                            var old = Read(addr, n);
                            UInt128? nw = at.Op switch
                            {
                                IrAtomicOp.Store or IrAtomicOp.Xchg => Val(at.A!),
                                IrAtomicOp.Add => Mask(at.Type, old + Val(at.A!)),
                                IrAtomicOp.Cas => old == Val(at.A!) ? Val(at.B!) : null,
                                _ => null,
                            };
                            if (nw is UInt128 w) Write(addr, n, w);
                            if (at.Dst != null) regs[at.Dst.Id] = old;
                            break;
                        }
                    default: throw new Trap("instruction");
                }
            }
            switch (blk.Term)
            {
                case IrJump j: blk = f.Blocks[j.Target]; break;
                case IrBranch b: blk = f.Blocks[Val(b.Cond) != 0 ? b.Then : b.Else]; break;
                case IrSwitch s:
                    {
                        var v = Val(s.Value);
                        int t = s.Default;
                        foreach (var (k, tgt) in s.Cases) if (k == v) { t = tgt; break; }
                        blk = f.Blocks[t];
                        break;
                    }
                case IrRet r: { var v = r.Value == null ? UInt128.Zero : Val(r.Value); _sp = savedSp; return v; }
                case IrRetAgg r:
                    {
                        uint s = P(r.Src);
                        Check(s, r.Size); Check(aggDst, r.Size);
                        Buffer.BlockCopy(_mem, (int)s, _mem, (int)aggDst, r.Size);
                        _sp = savedSp;
                        return aggDst;
                    }
                default: throw new Trap("terminator");
            }
        }
    }

    private UInt128 Binary(IrBinary b, UInt128 x, UInt128 y)
    {
        var t = b.Dst.Type;
        int bits = Bits(t);
        switch (b.Op)
        {
            case IrBinOp.Add: return Mask(t, x + y);
            case IrBinOp.Sub: return Mask(t, x - y);
            case IrBinOp.Mul: return Mask(t, x * y);
            case IrBinOp.And: return x & y;
            case IrBinOp.Or: return x | y;
            case IrBinOp.Xor: return x ^ y;
            case IrBinOp.Shl: return Mask(t, x << (int)(y % (UInt128)bits));
            case IrBinOp.LShr: return x >> (int)(y % (UInt128)bits);
            case IrBinOp.AShr: return Mask(t, (UInt128)(Signed(t, x) >> (int)(y % (UInt128)bits)));
            case IrBinOp.UDiv: if (y == 0) throw new Trap("divide by zero"); return x / y;
            case IrBinOp.URem: if (y == 0) throw new Trap("divide by zero"); return x % y;
            case IrBinOp.SDiv:
            case IrBinOp.SRem:
                {
                    if (y == 0) throw new Trap("divide by zero");
                    Int128 sx = Signed(t, x), sy = Signed(t, y);
                    Int128 min = bits == 128 ? Int128.MinValue : -((Int128)1 << (bits - 1));
                    if (sx == min && sy == -1) throw new Trap("signed divide overflow");
                    return Mask(t, (UInt128)(b.Op == IrBinOp.SDiv ? sx / sy : sx % sy));
                }
        }
        throw new Trap("binary");
    }

    private bool Compare(IrCompare c, UInt128 x, UInt128 y)
    {
        var t = IrPrinter.TypeOf(c.A);
        return c.Cc switch
        {
            IrCmp.Eq => x == y,
            IrCmp.Ne => x != y,
            IrCmp.Ult => x < y,
            IrCmp.Ule => x <= y,
            IrCmp.Ugt => x > y,
            IrCmp.Uge => x >= y,
            IrCmp.Slt => Signed(t, x) < Signed(t, y),
            IrCmp.Sle => Signed(t, x) <= Signed(t, y),
            IrCmp.Sgt => Signed(t, x) > Signed(t, y),
            IrCmp.Sge => Signed(t, x) >= Signed(t, y),
            _ => throw new Trap("compare"),
        };
    }
}
