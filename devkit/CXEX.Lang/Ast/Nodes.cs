using System.Collections.Generic;
using CXEX.Lang.Diagnostics;

namespace CXEX.Lang.Ast;

public abstract record Node { public SourceSpan Span { get; init; } }

// ---- program ----
public sealed record CompilationUnit(List<Decl> Decls) : Node;

// ---- declarations ----
public abstract record Decl : Node;
public sealed record Param(string Name, TypeRef Type) : Node;

public sealed record FnDecl(string Name, List<Param> Params, TypeRef Return, Block? Body) : Decl;
//   Body == null => extern fn
public sealed record StructDecl(string Name, List<Param> Fields) : Decl;
public sealed record ImportDecl(string Path) : Decl;   // import "file.xfxn";
public sealed record TypeAliasDecl(string Name, TypeRef Target) : Decl;   // type fx = i32;     // Param reused: name:type
public sealed record GlobalDecl(string Name, TypeRef Type, Expr? Init) : Decl;
public sealed record ConstDecl(string Name, TypeRef Type, Expr Value) : Decl;

// ---- statements ----
public abstract record Stmt : Node;
public sealed record Block(List<Stmt> Stmts) : Stmt;
public sealed record LetStmt(string Name, TypeRef? Type, Expr? Init) : Stmt;   // Init null = declared, uninitialized
public sealed record AssignStmt(Expr Target, Expr Value) : Stmt;
public sealed record IfStmt(Expr Cond, Block Then, Block? Else) : Stmt;
public sealed record WhileStmt(Expr Cond, Block Body) : Stmt;
public sealed record ReturnStmt(Expr? Value) : Stmt;
public sealed record BreakStmt() : Stmt;
public sealed record ContinueStmt() : Stmt;
public sealed record ExprStmt(Expr Expr) : Stmt;
public sealed record DeferStmt(Stmt Body) : Stmt;   // run Body when the enclosing block is left, by any route

// ---- expressions ----
public abstract record Expr : Node;
public sealed record IntLit(UInt128 Value) : Expr;
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

public enum UnOp { Neg, Not, Deref, AddrOf, BitNot }   // - ! * & ~
public enum BinOp
{
    Add, Sub, Mul, Div, Mod,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
    BitAnd, BitOr, BitXor, Shl, Shr,      // & | ^ << >>
}