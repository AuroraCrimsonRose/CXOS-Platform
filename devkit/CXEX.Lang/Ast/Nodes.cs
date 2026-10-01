using System.Collections.Generic;
using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Ast;

public abstract record Node { public SourceSpan Span { get; init; } }

// ---- program ----
public sealed record CompilationUnit(List<Decl> Decls) : Node;

// ---- declarations ----
/* Attributes: `@name` or `@name(args)` written before a declaration.
 *
 * Carried on the Decl base as an init-only list, so every declaration can have
 * them and no declaration's constructor had to change. What an attribute MEANS
 * is decided in one table in the type checker; an attribute that table does not
 * know is an error, never silently ignored - a typo like `@sectoin` that
 * compiled and quietly did nothing would be worse than having no attributes.
 *
 * An argument is positional (`@section(".x")`) or named (`@device(vendor =
 * 0x8086)`). Named arguments are parsed now even though no attribute takes
 * them yet: the syntax is the expensive part to change later, and hardware
 * matching tables will want them. */
public sealed record AttrArg(string? Name, Expr Value) : Node;
public sealed record Attr(string Name, List<AttrArg> Args) : Node;

public abstract record Decl : Node
{
    public List<Attr> Attrs { get; init; } = new();
}
public sealed record Param(string Name, TypeRef Type) : Node;

public sealed record FnDecl(string Name, List<Param> Params, TypeRef Return, Block? Body) : Decl;
//   Body == null => extern fn
public sealed record StructDecl(string Name, List<Param> Fields) : Decl;
public sealed record ImportDecl(string Path) : Decl;   // import "file.xfxn";
public sealed record TypeAliasDecl(string Name, TypeRef Target) : Decl;   // type fx = i32;     // Param reused: name:type
public sealed record GlobalDecl(string Name, TypeRef Type, Expr? Init) : Decl;
public sealed record ConstDecl(string Name, TypeRef Type, Expr Value) : Decl;

/* `enum color { red, green = 5, blue }` - a plain enum: its own integer type.
   `enum node { num { value: i64 }, eof }` - a SUM type: a tag and, for each
   variant, the fields it carries, reachable only through `switch`.
   Fields is null for a variant without a field list. */
public sealed record EnumVariant(string Name, List<Param>? Fields, Expr? Value) : Node;
public sealed record EnumDecl(string Name, TypeRef? Backing, List<EnumVariant> Variants) : Decl
{
    public bool IsSum => Variants.Exists(v => v.Fields != null);
    public int IndexOf(string variant) => Variants.FindIndex(v => v.Name == variant);
}

// ---- statements ----
public abstract record Stmt : Node;
public sealed record Block(List<Stmt> Stmts) : Stmt;
public sealed record LetStmt(string Name, TypeRef? Type, Expr? Init) : Stmt;   // Init null = declared, uninitialized
/* `x op= v` is parsed to AssignStmt(x, x op v) with Compound set: the type
   checker refuses a target containing a call (it would run twice), and lets
   the result narrow back into the target as C does. */
public sealed record AssignStmt(Expr Target, Expr Value) : Stmt { public bool Compound { get; init; } }
public sealed record IfStmt(Expr Cond, Block Then, Block? Else) : Stmt;
public sealed record WhileStmt(Expr Cond, Block Body) : Stmt;
public sealed record ReturnStmt(Expr? Value) : Stmt;
public sealed record BreakStmt() : Stmt;
public sealed record ContinueStmt() : Stmt;
public sealed record ExprStmt(Expr Expr) : Stmt;
public sealed record DeferStmt(Stmt Body) : Stmt;

/* switch (x) { case 1, 2 { } case node.num(n) { } else { } } - no fallthrough.
   Bind names a pointer to a variant's fields; BindLet is the local it lives in. */
public sealed record SwitchCase(List<Expr> Labels, string? Bind, Block Body) : Node { public LetStmt? BindLet { get; init; } }
public sealed record SwitchStmt(Expr Subject, List<SwitchCase> Cases, Block? Else) : Stmt;   // run Body when the enclosing block is left, by any route

// ---- expressions ----
public abstract record Expr : Node;
public sealed record IntLit(UInt128 Value) : Expr { public PrimKind? Suffix { get; init; } public bool IsChar { get; init; } }   // 1u64: Suffix = U64; 'a': U8, IsChar
public sealed record StrLit(string Value) : Expr;
public sealed record SizeofExpr(Expr Operand) : Expr;   // sizeof x -> u32, compile-time   // "..." -> *u8 into .rodata/.data
public sealed record BoolLit(bool Value) : Expr;
public sealed record NameExpr(string Name) : Expr;
public sealed record CallExpr(Expr Callee, List<Expr> Args) : Expr;
public sealed record MemberExpr(Expr Target, string Field) : Expr;     // s.field
public sealed record IndexExpr(Expr Target, Expr Index) : Expr;        // a[i]
public sealed record UnaryExpr(UnOp Op, Expr Operand) : Expr;          // - ! * &
public sealed record BinaryExpr(BinOp Op, Expr Left, Expr Right) : Expr;
public sealed record CastExpr(Expr Operand, TypeRef Target) : Expr;    // expr as T
public sealed record FieldInit(string Name, Expr Value) : Node;
/* `pt { x: 1, y: 2 }` - every field named. TypeName is the struct's name, or
   `Enum.variant` for a variant that carries fields. */
public sealed record StructLit(Expr TypeName, List<FieldInit> Fields) : Expr;
/* `[a, b, c]` - typed by the declaration it initialises, which fixes the count. */
public sealed record ArrayLit(List<Expr> Items) : Expr;

public enum UnOp { Neg, Not, Deref, AddrOf, BitNot }   // - ! * & ~
public enum BinOp
{
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
    BitAnd, BitOr, BitXor, Shl, Shr,      // & | ^ << >>
}