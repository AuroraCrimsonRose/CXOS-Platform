// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
﻿namespace CXEX.Lang.Lexer;

public enum TokenKind
{
    // literals & identifiers
    Identifier, IntLiteral, StringLiteral, True, False,
    // keywords
    Fn, Struct, Global, Const, Extern, Let, If, Else, While, Return, As,
    Sizeof, Break, Continue, Import, Type, Defer, Enum, Switch, Case,
    // punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Colon, Arrow,          // -> 
    // operators
    Assign,
    PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,   /* += -= *= /= %= */
    AmpAssign, PipeAssign, CaretAssign, ShlAssign, ShrAssign,           /* &= |= ^= <<= >>= */
    Plus, Minus, Star, Slash, Percent, Amp,
    Pipe, Caret, Tilde, Shl, Shr,            // | ^ ~ << >>
    Eq, Ne, Lt, Le, Gt, Ge,                  // == != < <= > >=
    AndAnd, OrOr, Not,                       // && || !
    Dot, At,                                 // . @
    // control
    Eof, Error
}