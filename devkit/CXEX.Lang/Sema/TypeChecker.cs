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
            else if (d is EnumDecl ed) { CheckEnum(ed); if (ed.IsSum) CheckSize(new NamedType(ed.Name), ed.Span); }
            else if (d is StructDecl sd) CheckSize(new NamedType(sd.Name), sd.Span);
            else if (d is GlobalDecl gd) CheckSize(gd.Type, gd.Span);
            else if (d is ConstDecl cd) CheckSize(cd.Type, cd.Span);
        foreach (var d in unit.Decls)
            switch (d)
            {
                case FnDecl f when f.Body != null:
                    _curReturn = f.Return; CheckBlock(f.Body); break;
                case ConstDecl c: _fold.TryEval(c.Value, out _); break;
                case GlobalDecl g when g.Init != null:
                    {
                        var it = CheckInit(g.Init, g.Type);
                        if (!Convertible(g.Type, g.Init, it))
                            _diag.Error($"cannot initialize global '{g.Name}' of type {Show(g.Type)} from {Show(it)}", g.Span);
                        CheckStaticInit(g.Init, g.Type);
                        break;
                    }
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
        {
            if (_ctx.Expand(p.Type) is ArrayType)
                _diag.Error($"parameter '{p.Name}' is an array; pass a pointer to it (*T) instead", p.Span);
            CheckSize(p.Type, p.Span);
        }
        if (_ctx.Expand(f.Return) is ArrayType)
            _diag.Error($"'{f.Name}' returns an array; return it through a pointer parameter instead", f.Span);
        CheckSize(f.Return, f.Span);
    }

    /* Every type written where it takes up room - a declaration, a local, a
       parameter, a cast - must have a size, and a sane one.
     *
     * A struct holding itself by value, directly or through others, has none:
     * sizing it recursed until the compiler crashed. And a type larger than
     * 1 GB cannot exist on this target: `let a: [0x7FFFFFFF]u32;` is 8 GB, and
     * the frame size it was added into wrapped negative, so the function
     * reserved no stack at all and wrote below it. 1 GB rather than 2 keeps
     * every sum of a few of them - a frame, a struct - clear of overflow. */
    private const int MaxObject = 1 << 30;

    private void CheckSize(TypeRef t, SourceSpan span)
    {
        _ctx.SizeCycle = null;
        int size = _ctx.SizeOf(t);
        if (_ctx.SizeCycle != null)
            _diag.Error($"'{_ctx.SizeCycle}' contains itself by value, so it has no size; use a pointer (*{_ctx.SizeCycle})", span);
        else if (size > MaxObject)
            _diag.Error($"{Show(t)} is larger than 1 GB, more than this 32-bit target can hold", span);
    }

    private bool IsSum(TypeRef t) =>
        _ctx.Expand(t) is NamedType n && _ctx.Enums.TryGetValue(n.Name, out var e) && e.IsSum;
    private EnumDecl? EnumOf(TypeRef t) =>
        _ctx.Expand(t) is NamedType n && _ctx.Enums.TryGetValue(n.Name, out var e) ? e : null;

    /* A plain enum is an integer of its backing type (u32 unless given) whose
       variants are constants that fit it and differ from one another. A sum
       type's tag is the variant's index, so it takes neither. */
    private void CheckEnum(EnumDecl ed)
    {
        if (ed.Variants.Count == 0) _diag.Error($"'{ed.Name}' has no variants", ed.Span);
        if (ed.IsSum)
        {
            if (ed.Backing != null) _diag.Error($"'{ed.Name}' is a sum type; its tag has no backing type to choose", ed.Span);
            foreach (var v in ed.Variants)
            {
                if (v.Value != null) _diag.Error($"a sum type's variants take no '= value' ('{v.Name}')", v.Span);
                if (v.Fields != null)
                    foreach (var f in v.Fields)
                        if (_ctx.Expand(f.Type) is NamedType fn && fn.Name == ed.Name)
                            _diag.Error($"'{ed.Name}.{v.Name}' contains '{ed.Name}' by value, which has no size; use a pointer (*{ed.Name})", f.Span);
            }
            return;
        }
        var backing = ed.Backing ?? U32;
        if (_ctx.Expand(backing) is not PrimType bp || !IsInt(backing) || PrimWidth.Bytes(bp.Kind) > 4)
        { _diag.Error($"an enum's backing type is an integer of at most 32 bits, not {Show(backing)}", ed.Span); return; }
        int bits = PrimWidth.Bytes(bp.Kind) * 8;
        bool sgn = PrimWidth.IsSigned(bp.Kind);
        var seen = new Dictionary<UInt128, string>();
        for (int i = 0; i < ed.Variants.Count; i++)
        {
            if (!_fold.TryEnumValue(ed, i, out var v)) continue;
            var name = ed.Variants[i].Name;
            /* in range for the backing type, reading the 128-bit fold as signed */
            Int128 sv = (Int128)v;
            Int128 lo = sgn ? -((Int128)1 << (bits - 1)) : 0;
            Int128 hi = sgn ? ((Int128)1 << (bits - 1)) - 1 : ((Int128)1 << bits) - 1;
            if (sv < lo || sv > hi) _diag.Error($"'{ed.Name}.{name}' = {sv} does not fit in {Show(backing)}", ed.Variants[i].Span);
            if (seen.TryGetValue(v, out var other))
                _diag.Error($"'{ed.Name}.{name}' has the same value as '{other}'", ed.Variants[i].Span);
            else seen[v] = name;
        }
    }

    /* switch: on an integer (up to 32 bits), a plain enum or a sum type.
       Labels are constants or variants of the subject's enum, each at most
       once. With no `else`, a switch on an enum or sum type must name every
       variant - adding a variant then finds every switch that forgot it. */
    private void CheckSwitch(SwitchStmt sw)
    {
        var st = CheckExpr(sw.Subject);
        var en = EnumOf(st);
        bool isInt = IsInt(st);
        if (isInt && IntWidth(st) > 4)
        { _diag.Error("switch on a 64 or 128-bit value is not supported; compare with if", sw.Subject.Span); isInt = false; }
        if (!isInt && en == null)
            _diag.Error($"cannot switch on {Show(st)}", sw.Subject.Span);

        var seenInts = new HashSet<UInt128>();
        var seenVariants = new HashSet<int>();
        foreach (var c in sw.Cases)
        {
            if (c.Bind != null && c.Labels.Count != 1)
                _diag.Error("a case that binds a variant's fields has exactly one label", c.Span);
            foreach (var l in c.Labels)
            {
                if (en != null)
                {
                    if (l is not MemberExpr lm || !_ctx.EnumRefs.TryGetValue(lm, out var er) || er.Enum != en)
                    { _diag.Error($"a case label here is a variant of '{en.Name}'", l.Span); continue; }
                    Set(l, new NamedType(en.Name));
                    if (!seenVariants.Add(er.Index))
                        _diag.Error($"'{en.Name}.{en.Variants[er.Index].Name}' appears in two cases", l.Span);
                    if (c.Bind != null)
                    {
                        var v = en.Variants[er.Index];
                        if (!en.IsSum || v.Fields is not { Count: > 0 })
                            _diag.Error($"'{en.Name}.{v.Name}' carries no fields to bind", l.Span);
                        else if (c.BindLet != null)
                            BindLocalType(c.BindLet, new PointerType(new NamedType(SemaContext.PayloadName(en.Name, v.Name))));
                    }
                }
                else if (isInt)
                {
                    if (c.Bind != null) _diag.Error("only a sum type's variant can bind fields", l.Span);
                    var lt = CheckExpr(l);
                    if (!new ConstFold(_ctx, _diag).TryEval(l, out var lv)) continue;
                    if (!IsInt(lt)) _diag.Error($"case label {Show(lt)} is not an integer", l.Span);
                    if (!seenInts.Add(lv)) _diag.Error("this value appears in two cases", l.Span);
                }
            }
            if (c.BindLet != null && !_localTypes.ContainsKey(c.BindLet)) BindLocalType(c.BindLet, Void);
            CheckBlock(c.Body);
        }
        if (sw.Else != null) CheckBlock(sw.Else);
        else if (en != null)
        {
            var missing = new List<string>();
            for (int i = 0; i < en.Variants.Count; i++)
                if (!seenVariants.Contains(i)) missing.Add($"{en.Name}.{en.Variants[i].Name}");
            if (missing.Count > 0)
                _diag.Error($"switch does not handle {string.Join(", ", missing)}; add the case(s), or an else", sw.Span);
        }
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

    private string NarrowHint(TypeRef to, TypeRef from, Expr? e = null)
    {
        if (e != null && IsInt(to) && FreeConst(e, out var v)) return $"; {(Int128)v} does not fit in {Show(to)}";
        return NarrowHintOf(to, from);
    }
    private string NarrowHintOf(TypeRef to, TypeRef from) =>
        !IsInt(to) || !IsInt(from) ? ""
        : WideNarrowing(to, from) ? $"; narrowing needs a cast: `as {Show(to)}`"
        : $"; {Show(from)} to {Show(to)} can change the value, so it needs a cast: `as {Show(to)}`";

    /* Can the value of `e` (of type `from`) go into a `to` without a cast?
     *
     * Between integers the spec's rule, which the checker did not enforce
     * until now - any integer went into any other and was cut down by the
     * store, so `let a: u8 = n;` with n = 300 quietly kept 44:
     *   - a CONSTANT goes anywhere its value fits (5 into a u8, -1 into an
     *     i8), unless it was written with a suffix, which fixes its type;
     *   - anything else only where no value can change: an unsigned value
     *     into a wider type, a signed one into a wider signed one.
     * Everything else is a cast the reader can see. */
    private bool Convertible(TypeRef to, Expr e, TypeRef from)
    {
        if (!IsInt(to) || !IsInt(from)) return Assignable(to, from);
        if (FreeConst(e, out var v)) return Fits(v, to);
        return ValuePreserving(to, from);
    }

    private bool ValuePreserving(TypeRef to, TypeRef from)
    {
        if (Same(to, from)) return true;
        if (_ctx.Expand(to) is not PrimType tp || _ctx.Expand(from) is not PrimType fp) return false;
        int wt = PrimWidth.Bytes(tp.Kind), wf = PrimWidth.Bytes(fp.Kind);
        bool st = PrimWidth.IsSigned(tp.Kind), sf = PrimWidth.IsSigned(fp.Kind);
        if (!sf) return st ? wt > wf : wt >= wf;
        return st && wt >= wf;
    }

    /* A constant whose type is not fixed by a suffix: the value is what
       matters, not the type its digits happened to get. A character literal
       counts - 'a' is a number, not a declaration that it is a u8. */
    private bool FreeConst(Expr e, out UInt128 value)
    {
        value = UInt128.Zero;
        if (HasSuffix(e)) return false;
        return new ConstFold(_ctx, new DiagnosticBag()).TryEval(e, out value);
    }

    private static bool HasSuffix(Expr e) => e switch
    {
        IntLit il => il.Suffix != null && !il.IsChar,
        UnaryExpr u => HasSuffix(u.Operand),
        BinaryExpr b => HasSuffix(b.Left) || HasSuffix(b.Right),
        CastExpr => true,          // an explicit type: keep it
        _ => false,
    };

    /* Does a folded constant fit `t`? The fold is 128-bit two's complement, so
       read it as signed - except into a u128, where every pattern is a value. */
    private bool Fits(UInt128 v, TypeRef t)
    {
        if (_ctx.Expand(t) is not PrimType p) return false;
        int bits = PrimWidth.Bytes(p.Kind) * 8;
        if (bits == 128) return true;
        Int128 sv = (Int128)v;
        if (PrimWidth.IsSigned(p.Kind))
            return sv >= -((Int128)1 << (bits - 1)) && sv <= ((Int128)1 << (bits - 1)) - 1;
        return sv >= 0 && sv <= ((Int128)1 << bits) - 1;
    }

    private TypeRef Set(Expr e, TypeRef t) { _ctx.Types[e] = t; return t; }

    private StructDecl? StructOf(TypeRef t0)
    {
        var t = _ctx.Expand(t0);
        return t is NamedType n && _ctx.Structs.TryGetValue(n.Name, out var s) ? s : null;
    }

    /* An expression checked against the type it is going into. Only an array
       literal needs this - `[1, 2, 3]` says nothing about its element type or
       length - but every place a value meets a declared type comes through
       here, so a literal can appear in any of them, nested ones included. */
    private TypeRef CheckInit(Expr e, TypeRef want)
    {
        if (e is ArrayLit al)
        {
            if (_ctx.Expand(want) is not ArrayType at)
            {
                _diag.Error($"an array literal cannot initialize {Show(want)}", al.Span);
                foreach (var i in al.Items) CheckExpr(i);
                return Set(e, Void);
            }
            if (al.Items.Count != at.Length)
                _diag.Error($"{Show(want)} needs {at.Length} item(s), given {al.Items.Count}", al.Span);
            foreach (var item in al.Items)
            {
                var it = CheckInit(item, at.Element);
                if (!Convertible(at.Element, item, it))
                    _diag.Error($"array item: {Show(it)} not assignable to {Show(at.Element)}{NarrowHint(at.Element, it, item)}", item.Span);
            }
            return Set(e, want);
        }
        return CheckExpr(e);
    }

    /* Every field, once, and nothing else. A field left out is an error rather
       than a silent zero: the point of naming them is that the reader sees
       every value the struct starts with. */
    private TypeRef CheckStructLit(StructLit sl)
    {
        /* Enum.variant { ... } builds a sum value of that variant. */
        if (sl.TypeName is MemberExpr vm && _ctx.EnumRefs.TryGetValue(vm, out var er))
        {
            var v = er.Enum.Variants[er.Index];
            if (v.Fields == null || !_ctx.Structs.TryGetValue(SemaContext.PayloadName(er.Enum.Name, v.Name), out var pd))
            {
                _diag.Error($"'{er.Enum.Name}.{v.Name}' carries no fields; write it without braces", sl.Span);
                foreach (var f in sl.Fields) CheckExpr(f.Value);
                return new NamedType(er.Enum.Name);
            }
            CheckFields(pd, sl.Fields, sl.Span);
            return new NamedType(er.Enum.Name);
        }
        if (sl.TypeName is not NameExpr tn || StructOf(new NamedType(tn.Name)) is not StructDecl sd)
        {
            _diag.Error("a record literal needs a struct name", sl.TypeName.Span);
            foreach (var f in sl.Fields) CheckExpr(f.Value);
            return Void;
        }
        return CheckFields(sd, sl.Fields, sl.Span) ? new NamedType(sd.Name) : new NamedType(sd.Name);
    }

    private bool CheckFields(StructDecl sd, List<FieldInit> given, SourceSpan span)
    {
        bool ok = true;
        var seen = new HashSet<string>();
        foreach (var fi in given)
        {
            var decl = sd.Fields.Find(f => f.Name == fi.Name);
            if (decl == null) { _diag.Error($"'{sd.Name}' has no field '{fi.Name}'", fi.Span); CheckExpr(fi.Value); ok = false; continue; }
            if (!seen.Add(fi.Name)) { _diag.Error($"field '{fi.Name}' given twice", fi.Span); ok = false; }
            var vt = CheckInit(fi.Value, decl.Type);
            if (!Convertible(decl.Type, fi.Value, vt))
            { _diag.Error($"field '{fi.Name}': {Show(vt)} not assignable to {Show(decl.Type)}{NarrowHint(decl.Type, vt, fi.Value)}", fi.Span); ok = false; }
        }
        foreach (var f in sd.Fields)
            if (!seen.Contains(f.Name)) { _diag.Error($"field '{f.Name}' of '{sd.Name}' is not given", span); ok = false; }
        return ok;
    }

    /* A global's initializer becomes bytes in the image, so everything in it
       must be known before the program runs: constants, strings (their
       address), function names (their address), and literals of those. */
    private void CheckStaticInit(Expr e, TypeRef t)
    {
        switch (e)
        {
            case StructLit sl:
                if (StructOf(t) is StructDecl sd)
                    foreach (var fi in sl.Fields)
                    {
                        var decl = sd.Fields.Find(f => f.Name == fi.Name);
                        if (decl != null) CheckStaticInit(fi.Value, decl.Type);
                    }
                break;
            case ArrayLit al:
                if (_ctx.Expand(t) is ArrayType at) foreach (var i in al.Items) CheckStaticInit(i, at.Element);
                break;
            case StrLit: break;
            case NameExpr n when _ctx.Resolved.TryGetValue(n, out var s) && s.Kind == SymKind.Function: break;
            default: _fold.TryEval(e, out _); break;
        }
    }

    private static bool ContainsCall(Expr e) => e switch
    {
        CallExpr => true,
        MemberExpr m => ContainsCall(m.Target),
        IndexExpr ix => ContainsCall(ix.Target) || ContainsCall(ix.Index),
        UnaryExpr u => ContainsCall(u.Operand),
        BinaryExpr b => ContainsCall(b.Left) || ContainsCall(b.Right),
        CastExpr c => ContainsCall(c.Operand),
        _ => false,
    };

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
                        if (l.Type != null) { CheckSize(l.Type, l.Span); BindLocalType(l, l.Type); }
                        break;
                    }
                    if (l.Type != null) CheckSize(l.Type, l.Span);
                    var it = l.Type != null ? CheckInit(l.Init!, l.Type) : CheckExpr(l.Init!);
                    if (l.Type != null)
                    {
                        if (!Convertible(l.Type, l.Init!, it))
                            _diag.Error($"cannot initialize '{l.Name}' of type {Show(l.Type)} from {Show(it)}{NarrowHint(l.Type, it, l.Init)}", l.Span);
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
                    var vt = CheckInit(a.Value, tt);
                    if (!IsLValue(a.Target)) { _diag.Error("assignment target is not assignable", a.Target.Span); break; }
                    if (a.Compound)
                    {
                        /* `x op= v` reads the target and writes it back, so it
                           is evaluated twice. That is harmless for a name, a
                           field or an index - and wrong for a call, which would
                           run twice. Refused rather than quietly done twice. */
                        if (ContainsCall(a.Target))
                            _diag.Error("the target of a compound assignment contains a call, which would run twice; " +
                                        "store the address in a local first", a.Target.Span);
                        /* The result narrows back into the target, as in C:
                           `b += 1` on a u8 is i32 arithmetic stored as a u8.
                           A WIDE result still needs `as` - see Assignable. */
                        if (!(IsInt(tt) && IsInt(vt) && !WideNarrowing(tt, vt)) && !Assignable(tt, vt))
                            _diag.Error($"cannot assign {Show(vt)} to {Show(tt)}{NarrowHint(tt, vt)}", a.Span);
                        break;
                    }
                    if (!Convertible(tt, a.Value, vt)) _diag.Error($"cannot assign {Show(vt)} to {Show(tt)}{NarrowHint(tt, vt, a.Value)}", a.Span);
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
                else { var rt = CheckInit(r.Value, _curReturn); if (!Convertible(_curReturn, r.Value, rt)) _diag.Error($"return type {Show(rt)} does not match {Show(_curReturn)}{NarrowHint(_curReturn, rt, r.Value)}", r.Span); }
                break;
            case ExprStmt e: CheckExpr(e.Expr); break;
            case SwitchStmt sw: CheckSwitch(sw); break;
        }
    }

    // LetStmt has no mutable field for the resolved local type; we re-bind via the
    // resolver's symbol by name lookup is unnecessary because CodeGen reads ctx.Types
    // for the init and the annotated/inferred type is recorded against the LetStmt's init.
    private readonly Dictionary<LetStmt, TypeRef> _localTypes = new();
    private void BindLocalType(LetStmt l, TypeRef t)
    {
        _localTypes[l] = t;
        if (_ctx.LetSymbols.TryGetValue(l, out var sym)) sym.Type = t;
    }
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
            case StructLit sl: return Set(e, CheckStructLit(sl));
            case ArrayLit al:
                _diag.Error("an array literal needs a declared array type to fill: `let a: [3]u32 = [1, 2, 3];`", al.Span);
                foreach (var i in al.Items) CheckExpr(i);
                return Set(e, Void);
            case SizeofExpr sz: CheckExpr(sz.Operand); return Set(e, U32);   // compile-time size  // "..." : *u8
            case BoolLit: return Set(e, Bool);

            case NameExpr n:
                if (n.Name == "__syscall") return Set(e, I32); // intrinsic callee placeholder
                return Set(e, _ctx.Resolved.TryGetValue(n, out var sym) ? sym.Type : Void);

            case CallExpr c: return Set(e, CheckCall(c));

            case MemberExpr em when _ctx.EnumRefs.TryGetValue(em, out var er):
                {
                    /* `color.red` is a value of type color. A sum type's variant
                       is a value too when it carries nothing; one with fields
                       has to be given them. */
                    var v = er.Enum.Variants[er.Index];
                    if (er.Enum.IsSum && v.Fields is { Count: > 0 })
                        _diag.Error($"'{er.Enum.Name}.{v.Name}' carries fields; give them: {er.Enum.Name}.{v.Name} {{ ... }}", em.Span);
                    return Set(e, new NamedType(er.Enum.Name));
                }
            case MemberExpr m:
                {
                    var tt = CheckExpr(m.Target);
                    if (IsSum(tt is PointerType sp ? sp.Pointee : tt))
                    {
                        _diag.Error("a sum type's fields are reached through a switch: case Type.variant(v) { v.field }", m.Span);
                        return Set(e, Void);
                    }
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
                    CheckSize(c.Target, c.Span);
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
                /* A constant takes the type of the other operand when it fits
                   it, as in Rust: `x + 1` on a u8 is a u8, not an i32 that then
                   needs a cast to go back into x. */
                bool lc = FreeConst(b.Left, out var lv), rc = FreeConst(b.Right, out var rv);
                if (lc && !rc && Fits(lv, r)) return r;
                if (rc && !lc && Fits(rv, l)) return l;
                return IntWidth(r) > IntWidth(l) ? r : l;
            case BinOp.Eq:
            case BinOp.Ne:
                if (!(IsInt(l) && IsInt(r)) && !(Assignable(l, r) || Assignable(r, l))) _diag.Error($"cannot compare {Show(l)} and {Show(r)}", b.Span);
                /* A struct is its address, so == would compare WHERE two values
                   are, not what they hold. Refused, not quietly wrong. */
                else if (StructOf(l) != null || _ctx.Expand(l) is ArrayType)
                    _diag.Error($"{Show(l)} cannot be compared with == or !=; compare the fields, or switch on a sum type", b.Span);
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
                var at = CheckInit(c.Args[i], fn.Params[i].Type);
                if (!Convertible(fn.Params[i].Type, c.Args[i], at))
                    _diag.Error($"arg {i + 1} to '{fn.Name}': {Show(at)} not assignable to {Show(fn.Params[i].Type)}{NarrowHint(fn.Params[i].Type, at, c.Args[i])}", c.Args[i].Span);
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
                var at = CheckInit(c.Args[i], ft.Params[i]);
                if (!Convertible(ft.Params[i], c.Args[i], at))
                    _diag.Error($"arg {i + 1}: {Show(at)} not assignable to {Show(ft.Params[i])}{NarrowHint(ft.Params[i], at, c.Args[i])}", c.Args[i].Span);
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