using System.Collections.Generic;
using CXEX.Lang.Ast;
using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Sema;

/// <summary>
/// Pass 2: assign a type to every expression and check statements. v0.1 is
/// deliberately lenient on integer width (any int coerces to any int) so the
/// language is usable before a full numeric-conversion policy exists; pointers,
/// bools, structs, and arrays are checked strictly. Records each expr's type in
/// ctx.Types for CodeGen. The __syscall intrinsic is the one effect primitive.
/// </summary>
public sealed class TypeChecker
{
    private readonly SemaContext _ctx;
    private readonly DiagnosticBag _diag;
    private readonly ConstFold _fold;
    private TypeRef _curReturn = new PrimType(PrimKind.Void);

    private static readonly PrimType I32 = new(PrimKind.I32);
    private static readonly PrimType U32 = new(PrimKind.U32);
    private static readonly PrimType Bool = new(PrimKind.Bool);
    private static readonly PrimType Void = new(PrimKind.Void);

    public TypeChecker(SemaContext ctx, DiagnosticBag diag)
    { _ctx = ctx; _diag = diag; _fold = new ConstFold(ctx, diag); }

    public void Check(CompilationUnit unit)
    {
        foreach (var d in unit.Decls) CheckAttrs(d);
        foreach (var d in unit.Decls)
            if (d is FnDecl fd) CheckSignature(fd);
        foreach (var d in unit.Decls)
            switch (d)
            {
                case FnDecl f when f.Body != null:
                    _curReturn = f.Return; CheckBlock(f.Body); break;
                case ConstDecl c: _fold.TryEval(c.Value, out _); break;
                case GlobalDecl g when g.Init != null: _fold.TryEval(g.Init, out _); break;
            }
    }

    /* Wide values and structs pass and return BY VALUE, as in C. An array
       does not: C cannot pass one either (a parameter declared as an array
       is really a pointer), and copying a buffer of any size onto the stack
       at every call is not something to do by accident. Say so, rather than
       quietly picking one of the two meanings. */
    private void CheckSignature(FnDecl f)
    {
        foreach (var p in f.Params)
            if (_ctx.Expand(p.Type) is ArrayType)
                _diag.Error($"parameter '{p.Name}' is an array; pass a pointer to it (*T) instead", p.Span);
        if (_ctx.Expand(f.Return) is ArrayType)
            _diag.Error($"'{f.Name}' returns an array; return it through a pointer parameter instead", f.Span);
    }

    // ---- attributes ----
    /* The one place an attribute gets its meaning. An attribute not handled
       here is an ERROR: silently ignoring an unknown name would let a typo
       compile into a program that quietly lacks whatever the attribute was
       for, and nothing would ever say so. Adding an attribute means adding a
       case here and teaching the emitter what it does - both, or neither. */
    private static readonly string[] KnownAttrs = { "section" };

    private void CheckAttrs(Decl d)
    {
        var seen = new HashSet<string>();
        foreach (var a in d.Attrs)
        {
            if (!seen.Add(a.Name))
            {
                _diag.Error($"'@{a.Name}' given more than once", a.Span);
                continue;
            }
            switch (a.Name)
            {
                case "section": CheckSection(d, a); break;
                default:
                    _diag.Error($"unknown attribute '@{a.Name}'; known: " +
                                string.Join(", ", KnownAttrs.Select(k => "@" + k)), a.Span);
                    break;
            }
        }
    }

    /* @section(".name") - put a function or global in a named object-file
       section instead of the default one.

       It is the mechanism that declarative tables are built on: many
       declarations, in many files, all landing in one section that something
       reads as an array. That is how driver match tables will work, without a
       hand-maintained registration list that drifts from the drivers.

       The default names are refused. `.text`, `.data`, `.rodata` and `.bss`
       already mean where a declaration goes by default, so naming one is either
       redundant or - a function in `.data` - contradictory, and the assembler
       would quietly merge section attributes rather than say so. */
    private static readonly HashSet<string> DefaultSections = new() { ".text", ".data", ".rodata", ".bss" };

    private void CheckSection(Decl d, Attr a)
    {
        switch (d)
        {
            case FnDecl { Body: null }:
                _diag.Error("'@section' on an extern function: it has no body to place", a.Span); return;
            case FnDecl: case GlobalDecl: break;
            default:
                _diag.Error($"'@section' does not apply to {DeclWord(d)}: only a function or a global " +
                            "occupies space in a section", a.Span);
                return;
        }
        if (a.Args.Count != 1 || a.Args[0].Name != null || a.Args[0].Value is not StrLit s)
        {
            _diag.Error("'@section' takes one argument, the section name as a string: @section(\".name\")", a.Span);
            return;
        }
        var name = s.Value;
        if (name.Length < 2 || name[0] != '.' ||
            !name.Skip(1).All(ch => char.IsAsciiLetterOrDigit(ch) || ch == '_' || ch == '.'))
        {
            _diag.Error($"section name '{name}' must start with '.' and contain only letters, digits, " +
                        "'_' and '.'", a.Span);
            return;
        }
        if (DefaultSections.Contains(name))
            _diag.Error($"'{name}' is where a declaration goes by default; '@section' is for a section of its own", a.Span);
    }

    private static string DeclWord(Decl d) => d switch
    {
        StructDecl => "a struct",
        ConstDecl => "a constant",
        TypeAliasDecl => "a type alias",
        ImportDecl => "an import",
        _ => "this declaration",
    };

    // ---- helpers ----
    // all three expand type aliases first, so `type fx = i32;` behaves as i32
    private bool IsInt(TypeRef t0)
    {
        var t = _ctx.Expand(t0);
        /* Every integer width, narrow or wide. Listing them by name here was
           what made u64 and u128 invisible to arithmetic even after the parser
           and emitter knew about them - a new width is now one row in
           PrimWidth and nothing else. */
        return t is PrimType p && p.Kind is not (PrimKind.Bool or PrimKind.Void);
    }
    /* Width in bytes of an integer type, for deciding which operand of a
       binary expression is the wider. 0 for anything that is not a primitive,
       which cannot win the comparison and so keeps the left type. */
    private int IntWidth(TypeRef t0) =>
        _ctx.Expand(t0) is PrimType p ? PrimWidth.Bytes(p.Kind) : 0;

    private bool IsBool(TypeRef t0) => _ctx.Expand(t0) is PrimType { Kind: PrimKind.Bool };
    private bool IsPtr(TypeRef t0) => _ctx.Expand(t0) is PointerType;

    private bool SameFunc(FuncType x, FuncType y)
    {
        if (x.Params.Count != y.Params.Count) return false;
        if (!Same(x.Return, y.Return)) return false;
        for (int i = 0; i < x.Params.Count; i++)
            if (!Same(x.Params[i], y.Params[i])) return false;
        return true;
    }

    // aliases expand first, so `type fx = i32;` compares equal to i32
    private bool Same(TypeRef a0, TypeRef b0)
    {
        var a = _ctx.Expand(a0); var b = _ctx.Expand(b0);
        return (a, b) switch
        {
            (PrimType x, PrimType y) => x.Kind == y.Kind,
            // Different address spaces are different types. This one line is
            // what makes assignment, argument passing, returning and `==`
            // between a *phys and a *u8 a compile error, since all of them
            // go through Same().
            (PointerType x, PointerType y) => x.Space == y.Space && Same(x.Pointee, y.Pointee),
            (NamedType x, NamedType y) => x.Name == y.Name,
            (ArrayType x, ArrayType y) => x.Length == y.Length && Same(x.Element, y.Element),
            (FuncType x, FuncType y) => SameFunc(x, y),
            _ => false
        };
    }

    // assignable: ints interchange (v0.1); else exact; ptr<-ptr exact pointee
    private bool Assignable(TypeRef to, TypeRef from)
        => (IsInt(to) && IsInt(from) && !WideNarrowing(to, from)) || Same(to, from);

    /* A 64 or 128-bit value going somewhere narrower, with no `as`. It used to
       be allowed, and a wide value is its ADDRESS, so `let a: u32 = w;` stored
       the address - in a let, an assignment, an argument and a return alike.
       The spec has always said narrowing needs a cast; for these widths it now
       does, because dropping the top of a 64-bit file offset without a word is
       exactly the bug the wide types exist to prevent. */
    private bool WideNarrowing(TypeRef to, TypeRef from) =>
        IntWidth(from) > 4 && IntWidth(from) > IntWidth(to);

    private string NarrowHint(TypeRef to, TypeRef from) =>
        IsInt(to) && WideNarrowing(to, from) ? $"; narrowing needs a cast: `as {Show(to)}`" : "";

    private TypeRef Set(Expr e, TypeRef t) { _ctx.Types[e] = t; return t; }

    private StructDecl? StructOf(TypeRef t0)
    {
        var t = _ctx.Expand(t0);
        return t is NamedType n && _ctx.Structs.TryGetValue(n.Name, out var s) ? s : null;
    }

    private static bool IsLValue(Expr e) => e is NameExpr or UnaryExpr { Op: UnOp.Deref } or IndexExpr or MemberExpr;

    // ---- statements ----
    private void CheckBlock(Block b) { foreach (var s in b.Stmts) CheckStmt(s); }

    private int _loopDepth;   // break/continue must appear inside a loop
    private IntLit? _negatedLiteral;   // the literal directly under a unary '-', if checking one
    private int _deferDepth;  // and nothing may leave a defer body early

    private void CheckStmt(Stmt s)
    {
        switch (s)
        {
            case Block b: CheckBlock(b); break;

            case LetStmt l:
                {
                    if (l.Init == null)
                    {
                        // `let x: T;` - declared, uninitialized; type required.
                        if (l.Type != null) BindLocalType(l, l.Type);
                        break;
                    }
                    var it = CheckExpr(l.Init!);
                    if (l.Type != null)
                    {
                        if (!Assignable(l.Type, it))
                            _diag.Error($"cannot initialize '{l.Name}' of type {Show(l.Type)} from {Show(it)}{NarrowHint(l.Type, it)}", l.Span);
                        BindLocalType(l, l.Type);
                    }
                    else BindLocalType(l, it == Void ? I32 : it);
                    break;
                }
            case BreakStmt:
                if (_loopDepth == 0)
                    _diag.Error(_deferDepth > 0 ? "cannot 'break' out of a defer" : "'break' outside a loop", s.Span);
                break;
            case ContinueStmt:
                if (_loopDepth == 0)
                    _diag.Error(_deferDepth > 0 ? "cannot 'continue' out of a defer" : "'continue' outside a loop", s.Span);
                break;
            case DeferStmt d:
                {
                    /* A defer body runs while the block is ALREADY being left -
                       on the way out of a return, a break, or the end of the
                       block. So it may not itself leave by one of those
                       routes: a `return` inside a defer would abandon the
                       return that is running it, and the cleanup after it
                       would silently not happen. Loops and blocks wholly
                       inside the body are fine; it is only escaping the
                       defer that is refused, which is why the loop depth is
                       zeroed here rather than just a flag set. */
                    if (d.Body is LetStmt)
                        _diag.Error("'defer let' declares a variable nothing can use; defer a statement or a block", d.Span);
                    int savedLoops = _loopDepth;
                    _loopDepth = 0;
                    _deferDepth++;
                    CheckStmt(d.Body);
                    _deferDepth--;
                    _loopDepth = savedLoops;
                    break;
                }
            case AssignStmt a:
                {
                    var tt = CheckExpr(a.Target);
                    var vt = CheckExpr(a.Value);
                    if (!IsLValue(a.Target)) _diag.Error("assignment target is not assignable", a.Target.Span);
                    else if (!Assignable(tt, vt)) _diag.Error($"cannot assign {Show(vt)} to {Show(tt)}{NarrowHint(tt, vt)}", a.Span);
                    break;
                }
            case IfStmt i:
                Expect(CheckExpr(i.Cond), Bool, i.Cond.Span, "if condition");
                CheckBlock(i.Then); if (i.Else != null) CheckBlock(i.Else); break;
            case WhileStmt w:
                Expect(CheckExpr(w.Cond), Bool, w.Cond.Span, "while condition");
                _loopDepth++; CheckBlock(w.Body); _loopDepth--; break;
            case ReturnStmt r:
                if (_deferDepth > 0) _diag.Error("cannot 'return' from inside a defer", r.Span);
                if (r.Value == null) { if (!IsBool(_curReturn) && _curReturn is not PrimType { Kind: PrimKind.Void }) _diag.Error("return requires a value", r.Span); }
                else { var rt = CheckExpr(r.Value); if (!Assignable(_curReturn, rt)) _diag.Error($"return type {Show(rt)} does not match {Show(_curReturn)}{NarrowHint(_curReturn, rt)}", r.Span); }
                break;
            case ExprStmt e: CheckExpr(e.Expr); break;
        }
    }

    // LetStmt has no mutable field for the resolved local type; we re-bind via the
    // resolver's symbol by name lookup is unnecessary because CodeGen reads ctx.Types
    // for the init and the annotated/inferred type is recorded against the LetStmt's init.
    private readonly Dictionary<LetStmt, TypeRef> _localTypes = new();
    private void BindLocalType(LetStmt l, TypeRef t) => _localTypes[l] = t;
    public IReadOnlyDictionary<LetStmt, TypeRef> LocalTypes => _localTypes;

    // ---- expressions ----
    private TypeRef CheckExpr(Expr e)
    {
        switch (e)
        {
            /* A literal is typed by the narrowest type that HOLDS it.
             *
             * Anything fitting in 32 bits is still i32, so nothing that
             * compiled before changes shape. What this fixes is the case above
             * that: every literal used to type as i32 however large it was
             * written, so `200000000000000000000 % 7` took the narrow path,
             * truncated the literal into eax and did a 32-bit divide - a wrong
             * answer from a constant expression, with nothing to warn on. A
             * literal too wide for a register now types wide and is emitted
             * through the wide path that can actually carry it.
             *
             * Unsigned, because a literal is a magnitude; unary minus is a
             * separate node applied to it. */
            /* `int.MaxValue`, not `uint.MaxValue`: 0x80000000 through
               0xFFFFFFFF do not fit in an i32, and typing them as one made
               `y < 0x80000000` on an i32 compare against -2147483648. They are
               u32, the narrowest type that actually holds them - which is also
               what C does with an unsuffixed hex constant that large. */
            /* A suffix names the type outright, and the value must fit it: a
               literal that silently wrapped into its own declared type would
               be the one kind of constant that cannot be trusted to be what
               it says. A signed literal may reach one past its maximum when
               it is negated directly, so -128i8 and the most negative i128
               can be written at all. */
            case IntLit { Suffix: PrimKind sk } sl:
                {
                    int bits = PrimWidth.Bytes(sk) * 8;
                    bool signedK = PrimWidth.IsSigned(sk);
                    UInt128 max = signedK ? (UInt128.One << (bits - 1)) - 1
                                : bits == 128 ? UInt128.MaxValue : (UInt128.One << bits) - 1;
                    if (signedK && ReferenceEquals(sl, _negatedLiteral)) max += 1;
                    if (sl.Value > max)
                        _diag.Error($"{sl.Value} does not fit in {sk.ToString().ToLowerInvariant()}", sl.Span);
                    return Set(e, new PrimType(sk));
                }
            case IntLit il:
                return Set(e, il.Value <= int.MaxValue ? I32
                            : il.Value <= uint.MaxValue ? U32
                            : il.Value <= ulong.MaxValue ? new PrimType(PrimKind.U64)
                            : new PrimType(PrimKind.U128));
            case StrLit: return Set(e, new PointerType(new PrimType(PrimKind.U8)));
            case SizeofExpr sz: CheckExpr(sz.Operand); return Set(e, U32);   // compile-time size  // "..." : *u8
            case BoolLit: return Set(e, Bool);

            case NameExpr n:
                if (n.Name == "__syscall") return Set(e, I32); // intrinsic callee placeholder
                return Set(e, _ctx.Resolved.TryGetValue(n, out var sym) ? sym.Type : Void);

            case CallExpr c: return Set(e, CheckCall(c));

            case MemberExpr m:
                {
                    var tt = CheckExpr(m.Target);
                    if (!CheckDeref(tt, m.Span)) return Set(e, Void);
                    var sd = StructOf(tt is PointerType p ? p.Pointee : tt); // allow s.f and ps.f
                    if (sd == null) { _diag.Error($"'.{m.Field}' on non-struct {Show(tt)}", m.Span); return Set(e, Void); }
                    foreach (var f in sd.Fields) if (f.Name == m.Field) return Set(e, f.Type);
                    _diag.Error($"struct '{sd.Name}' has no field '{m.Field}'", m.Span); return Set(e, Void);
                }
            case IndexExpr ix:
                {
                    var tt = CheckExpr(ix.Target);
                    Expect(CheckExpr(ix.Index), I32, ix.Index.Span, "index");
                    if (!CheckDeref(tt, ix.Span)) return Set(e, Void);
                    TypeRef elem = tt switch { PointerType p => p.Pointee, ArrayType a => a.Element, _ => null! };
                    if (elem == null) { _diag.Error($"cannot index {Show(tt)}", ix.Span); return Set(e, Void); }
                    return Set(e, elem);
                }
            case UnaryExpr u:
                {
                    if (u.Op == UnOp.Neg && u.Operand is IntLit nl) _negatedLiteral = nl;
                    var ot = CheckExpr(u.Operand);
                    _negatedLiteral = null;
                    switch (u.Op)
                    {
            case UnOp.BitNot:
            case UnOp.Neg: if (!IsInt(ot)) _diag.Error("unary '-' / '~' needs an integer", u.Span); return Set(e, ot);
                        case UnOp.Not: if (!IsBool(ot)) _diag.Error("unary '!' needs a bool", u.Span); return Set(e, Bool);
                        case UnOp.Deref:
                            if (ot is PointerType p) return CheckDeref(ot, u.Span) ? Set(e, p.Pointee) : Set(e, Void);
                            _diag.Error("cannot dereference non-pointer", u.Span); return Set(e, Void);
                        case UnOp.AddrOf: return Set(e, new PointerType(ot));
                    }
                    return Set(e, Void);
                }
            case BinaryExpr b: return Set(e, CheckBinary(b));
            case CastExpr c:
                {
                    var from = CheckExpr(c.Operand);
                    /* Casts are otherwise permissive (v0.1), with ONE exception:
                       a pointer may not be cast straight to a pointer in a
                       different address space.

                       `p as *u8` on a *phys is not a conversion, it is a lie -
                       the number is unchanged, and it names different memory
                       than a virtual pointer with that number would. What
                       every real crossing actually does is arithmetic: phys to
                       virt adds an offset, validating a user pointer checks a
                       range. So the route is through an integer -
                       `(p as u32 + OFFSET) as *u8` - which is exactly where
                       that arithmetic goes, and which makes an untranslated
                       crossing (`p as u32 as *u8`) something a reader can see
                       was deliberate rather than something a cast hid.

                       Pointer to integer and integer to pointer stay free, in
                       any space: that is how an address arrives from a device
                       register or a syscall argument in the first place. */
                    if (_ctx.Expand(from) is PointerType pf && _ctx.Expand(c.Target) is PointerType pt &&
                        pf.Space != pt.Space)
                    {
                        _diag.Error($"cannot cast {Show(from)} to {Show(c.Target)}: a pointer does not " +
                                    "change address space by being relabelled. Convert through an integer, " +
                                    "where the translation belongs", c.Span);
                    }
                    return Set(e, c.Target);
                }

            default: return Set(e, Void);
        }
    }

    private TypeRef CheckBinary(BinaryExpr b)
    {
        var l = CheckExpr(b.Left); var r = CheckExpr(b.Right);
        switch (b.Op)
        {
            case BinOp.Add:
            case BinOp.Sub:
            case BinOp.Mul:
            case BinOp.Div:
            case BinOp.Mod:
            case BinOp.BitAnd:
            case BinOp.BitOr:
            case BinOp.BitXor:
            case BinOp.Shl:
            case BinOp.Shr:
                if (!IsInt(l) || !IsInt(r)) _diag.Error($"arithmetic on {Show(l)} and {Show(r)}", b.Span);
                if (!IsInt(l)) return IsInt(r) ? r : I32;
                if (!IsInt(r)) return l;
                /* The WIDER operand decides, not the left one.
                 *
                 * This used to return the left type unconditionally, which
                 * made `0 - a` on a 64-bit `a` type as i32 - and a wide value
                 * lives at an address, so the emitter then did 32-bit
                 * arithmetic on that ADDRESS and sign-extended the result.
                 * `a - 0` was correct and `0 - a` was garbage, silently. That
                 * is precisely the class of error wide types exist to prevent,
                 * so the rule is the wider operand wins.
                 *
                 * A shift is the exception: its right operand is a COUNT, not
                 * a term, so `x << n` is as wide as x however n is written. */
                if (b.Op is BinOp.Shl or BinOp.Shr) { CheckShiftAmount(b, l); return l; }
                return IntWidth(r) > IntWidth(l) ? r : l;
            case BinOp.Eq:
            case BinOp.Ne:
                if (!(Assignable(l, r) || Assignable(r, l))) _diag.Error($"cannot compare {Show(l)} and {Show(r)}", b.Span);
                return Bool;
            case BinOp.Lt:
            case BinOp.Le:
            case BinOp.Gt:
            case BinOp.Ge:
                if (!IsInt(l) || !IsInt(r)) _diag.Error($"ordering on {Show(l)} and {Show(r)}", b.Span);
                return Bool;
            case BinOp.And:
            case BinOp.Or:
                if (!IsBool(l) || !IsBool(r)) _diag.Error("logical operator needs bools", b.Span);
                return Bool;
        }
        return I32;
    }

    /* A CONSTANT shift amount must be inside the width the shift is done at:
       32 bits for anything up to an int (it happens in a register), the type's
       own width above that. Past it, the CPU quietly reduces the amount modulo
       32, so `1 << 100` became 1 << 4 = 16 - with the spec claiming it was
       refused. A variable amount is reduced modulo the width on purpose; a
       constant one that large is a mistake. */
    private void CheckShiftAmount(BinaryExpr b, TypeRef l)
    {
        if (!new ConstFold(_ctx, new DiagnosticBag()).TryEval(b.Right, out var amt)) return;
        int width = Math.Max(32, IntWidth(l) * 8);
        if (amt < (UInt128)width) return;
        string hint = b.Left is IntLit { Suffix: null } && amt < 128
            ? $"; to shift at a wider type, give the literal a suffix: {((IntLit)b.Left).Value}u{(amt < 64 ? 64 : 128)} {(b.Op == BinOp.Shl ? "<<" : ">>")} {amt}"
            : "";
        _diag.Error($"shift of {amt} is outside 0..{width - 1} for {Show(l)}{hint}", b.Span);
    }

    private TypeRef CheckCall(CallExpr c)
    {
        // __syscall intrinsic: __syscall(n, a1..a5) -> i32
        if (c.Callee is NameExpr { Name: "__syscall" })
        {
            if (c.Args.Count < 1 || c.Args.Count > 6)
                _diag.Error("__syscall takes the number plus up to 5 args", c.Span);
            foreach (var a in c.Args) { var at = CheckExpr(a); if (!IsInt(at) && !IsPtr(at)) _diag.Error("__syscall args must be integer/pointer", a.Span); }
            return I32;
        }

        var ct = CheckExpr(c.Callee);
        if (c.Callee is NameExpr nm && _ctx.Resolved.TryGetValue(nm, out var sym) && sym.Decl is FnDecl fn)
        {
            if (c.Args.Count != fn.Params.Count)
                _diag.Error($"'{fn.Name}' expects {fn.Params.Count} args, got {c.Args.Count}", c.Span);
            for (int i = 0; i < c.Args.Count && i < fn.Params.Count; i++)
            {
                var at = CheckExpr(c.Args[i]);
                if (!Assignable(fn.Params[i].Type, at))
                    _diag.Error($"arg {i + 1} to '{fn.Name}': {Show(at)} not assignable to {Show(fn.Params[i].Type)}{NarrowHint(fn.Params[i].Type, at)}", c.Args[i].Span);
            }
            return fn.Return;
        }
        // calling through a function pointer: `let f: fn(i32)->i32 = ...; f(3)`
        if (_ctx.Expand(ct) is FuncType ft)
        {
            if (c.Args.Count != ft.Params.Count)
                _diag.Error($"call expects {ft.Params.Count} args, got {c.Args.Count}", c.Span);
            for (int i = 0; i < c.Args.Count && i < ft.Params.Count; i++)
            {
                var at = CheckExpr(c.Args[i]);
                if (!Assignable(ft.Params[i], at))
                    _diag.Error($"arg {i + 1}: {Show(at)} not assignable to {Show(ft.Params[i])}{NarrowHint(ft.Params[i], at)}", c.Args[i].Span);
            }
            return ft.Return;
        }

        foreach (var a in c.Args) CheckExpr(a);
        _diag.Error("call target is not a function", c.Span);
        return Void;
    }

    private void Expect(TypeRef got, TypeRef want, SourceSpan span, string what)
    {
        if (!Assignable(want, got)) _diag.Error($"{what} must be {Show(want)}, got {Show(got)}", span);
    }

    private static string SpaceWord(AddrSpace s) => s switch
    {
        AddrSpace.User => "user ",
        AddrSpace.Phys => "phys ",
        AddrSpace.Dma  => "dma ",
        _              => "",
    };

    /* Refuse to read or write THROUGH a pointer that is not valid in this
     * address space. Called by all three forms of dereference - `*p`, `p[i]`
     * and `p.field` - so none of them can become the way around the others.
     *
     * Each message says why, because "cannot dereference" alone would read as
     * an arbitrary rule. The point is that the number in a *phys or *dma
     * pointer names a DIFFERENT memory than the CPU would reach by using it,
     * and the number in a *user pointer is one a less-trusted caller chose. */
    private bool CheckDeref(TypeRef t, SourceSpan span)
    {
        if (_ctx.Expand(t) is not PointerType { Space: not AddrSpace.Normal } p) return true;
        _diag.Error(p.Space switch
        {
            AddrSpace.User =>
                "cannot dereference a *user pointer: the address came from a less-trusted " +
                "caller and has not been validated. Check it, then convert through an integer",
            AddrSpace.Phys =>
                "cannot dereference a *phys pointer: a physical address is not mapped at that " +
                "number. Translate it to a virtual address first",
            _ =>
                "cannot dereference a *dma pointer: it is the address a device sees, not the " +
                "one the CPU does",
        }, span);
        return false;
    }

    private string Show(TypeRef t) => t switch
    {
        PrimType p => p.Kind.ToString().ToLowerInvariant(),
        PointerType p => "*" + SpaceWord(p.Space) + Show(p.Pointee),
        ArrayType a => $"[{a.Length}]" + Show(a.Element),
        NamedType n => n.Name,
        _ => "?"
    };
}