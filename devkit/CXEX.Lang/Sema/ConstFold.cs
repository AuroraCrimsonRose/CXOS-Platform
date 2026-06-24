using CXEX.Lang.Ast;
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
    public ConstFold(SemaContext ctx, DiagnosticBag diag) { _ctx = ctx; _diag = diag; }

    public bool TryEval(Expr e, out ulong value)
    {
        value = 0;
        switch (e)
        {
            case IntLit i: value = i.Value; return true;
            case BoolLit b: value = b.Value ? 1u : 0u; return true;

            case NameExpr n:
                if (_ctx.Resolved.TryGetValue(n, out var sym) && sym.Kind == SymKind.Const &&
                    sym.Decl is ConstDecl cd)
                    return TryEval(cd.Value, out value);
                _diag.Error($"'{(e as NameExpr)?.Name}' is not a constant", e.Span);
                return false;

            case CastExpr c: return TryEval(c.Operand, out value); // v0.1: value-preserving

            case UnaryExpr u when TryEval(u.Operand, out var v):
                value = u.Op switch { UnOp.Neg => (ulong)(-(long)v), UnOp.Not => v == 0 ? 1u : 0u, _ => v };
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
                    case BinOp.Eq: value = l == r ? 1u : 0u; return true;
                    case BinOp.Ne: value = l != r ? 1u : 0u; return true;
                    case BinOp.Lt: value = l < r ? 1u : 0u; return true;
                    case BinOp.Le: value = l <= r ? 1u : 0u; return true;
                    case BinOp.Gt: value = l > r ? 1u : 0u; return true;
                    case BinOp.Ge: value = l >= r ? 1u : 0u; return true;
                    case BinOp.And: value = (l != 0 && r != 0) ? 1u : 0u; return true;
                    case BinOp.Or: value = (l != 0 || r != 0) ? 1u : 0u; return true;
                }
                return false;

            default:
                _diag.Error("expected a constant expression", e.Span);
                return false;
        }
    }
}