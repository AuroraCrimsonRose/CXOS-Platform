namespace CXEX.Lang.Lexer;

public enum TokenKind
{
    // literals & identifiers
    Identifier, IntLiteral, StringLiteral, True, False,
    // keywords
    Fn, Struct, Global, Const, Extern, Let, If, Else, While, Return, As,
    Sizeof, Break, Continue, Import, Type, Defer,
    // punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Colon, Arrow,          // -> 
    // operators
    Assign, Plus, Minus, Star, Slash, Percent, Amp,
    Pipe, Caret, Tilde, Shl, Shr,            // | ^ ~ << >>
    Eq, Ne, Lt, Le, Gt, Ge,                  // == != < <= > >=
    AndAnd, OrOr, Not,                       // && || !
    Dot, At,                                 // . @
    // control
    Eof, Error
}