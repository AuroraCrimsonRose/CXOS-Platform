// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
using System.Collections.Generic;
﻿using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Sema;

/// <summary>
/// Compile-time constant evaluation for X core v0.1: integer/bool literals, the
/// arithmetic/comparison/logical operators, unary neg/not, casts (value-preserving),
/// and references to other consts. Used for const initializers, global initializers,
/// and array lengths. Returns false (with a diagnostic) for anything not constant.
/// </summary>
public sealed class ConstFold
{
    private readonly SemaContext _ctx;
    private readonly DiagnosticBag _diag;
    private readonly HashSet<ConstDecl> _evaluating = new();
    public ConstFold(SemaContext ctx, DiagnosticBag diag) { _ctx = ctx; _diag = diag; }

    /* A variant's value: its own `= expr`, else one more than the variant
       before it, starting from 0. A sum type's tag is simply its index. */
    public bool TryEnumValue(EnumDecl ed, int index, out UInt128 value)
    {
        value = UInt128.Zero;
        if (ed.IsSum) { value = (UInt128)index; return true; }
        for (int i = 0; i <= index; i++)
        {
            var v = ed.Variants[i];
            if (v.Value != null) { if (!TryEval(v.Value, out value)) return false; }
            else if (i > 0) value += 1;
        }
        return true;
    }

    public bool TryEval(Expr e, out UInt128 value)
    {
        value = UInt128.Zero;
        switch (e)
        {
            case IntLit i: value = i.Value; return true;
            case BoolLit b: value = b.Value ? UInt128.One : UInt128.Zero; return true;

            case NameExpr n:
                if (_ctx.Resolved.TryGetValue(n, out var sym) && sym.Kind == SymKind.Const &&
                    sym.Decl is ConstDecl cd)
                {
                    /* `const a: u32 = b; const b: u32 = a;` has no value - and
                       following it recursed until the compiler crashed. */
                    if (!_evaluating.Add(cd)) { _diag.Error($"'{n.Name}' is defined in terms of itself", e.Span); return false; }
                    try { return TryEval(cd.Value, out value); }
                    finally { _evaluating.Remove(cd); }
                }
                _diag.Error($"'{(e as NameExpr)?.Name}' is not a constant", e.Span);
                return false;

            case CastExpr c: return TryEval(c.Operand, out value); // v0.1: value-preserving

            case MemberExpr m when _ctx.EnumRefs.TryGetValue(m, out var er):
                return TryEnumValue(er.Enum, er.Index, out value);

            case UnaryExpr u when TryEval(u.Operand, out var v):
                /* ~ was missing, and fell into the default that returns the
                   operand: `const M: u32 = ~0;` folded to 0, not 0xFFFFFFFF. */
                value = u.Op switch { UnOp.Neg => UInt128.Zero - v, UnOp.Not => v == 0 ? UInt128.One : UInt128.Zero,
                                      UnOp.BitNot => ~v, _ => v };
                if (u.Op is UnOp.Deref or UnOp.AddrOf) { _diag.Error("non-constant expression", e.Span); return false; }
                return true;

            case BinaryExpr b when TryEval(b.Left, out var l) & TryEval(b.Right, out var r):
                switch (b.Op)
                {
                    case BinOp.Add: value = l + r; return true;
                    case BinOp.Sub: value = l - r; return true;
                    case BinOp.Mul: value = l * r; return true;
                    case BinOp.Div: if (r == 0) { _diag.Error("constant divide by zero", e.Span); return false; } value = l / r; return true;
                    case BinOp.Mod: if (r == 0) { _diag.Error("constant modulo by zero", e.Span); return false; } value = l % r; return true;
                    case BinOp.Eq: value = l == r ? UInt128.One : UInt128.Zero; return true;
                    case BinOp.Ne: value = l != r ? UInt128.One : UInt128.Zero; return true;
                    case BinOp.Lt: value = l < r ? UInt128.One : UInt128.Zero; return true;
                    case BinOp.Le: value = l <= r ? UInt128.One : UInt128.Zero; return true;
                    case BinOp.Gt: value = l > r ? UInt128.One : UInt128.Zero; return true;
                    case BinOp.Ge: value = l >= r ? UInt128.One : UInt128.Zero; return true;
                    case BinOp.And: value = (l != 0 && r != 0) ? UInt128.One : UInt128.Zero; return true;
                    case BinOp.Or: value = (l != 0 || r != 0) ? UInt128.One : UInt128.Zero; return true;
                    /* Bitwise and shift were missing entirely, so a constant
                       as ordinary as `1 | 2` did not fold and was reported as
                       "expected a constant expression" - which names the
                       symptom and not the cause. */
                    case BinOp.BitAnd: value = l & r; return true;
                    case BinOp.BitOr:  value = l | r; return true;
                    case BinOp.BitXor: value = l ^ r; return true;
                    case BinOp.Shl: if (r >= 128) { _diag.Error("constant shift of 128 or more", e.Span); return false; } value = l << (int)r; return true;
                    case BinOp.Shr: if (r >= 128) { _diag.Error("constant shift of 128 or more", e.Span); return false; } value = l >> (int)r; return true;
                }
                return false;

            default:
                _diag.Error("expected a constant expression", e.Span);
                return false;
        }
    }
}