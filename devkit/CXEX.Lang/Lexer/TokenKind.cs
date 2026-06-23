namespace CXEX.Lang.Lexer;

public enum TokenKind
{
    // literals & identifiers
    Identifier, IntLiteral, True, False,
    // keywords
    Fn, Struct, Global, Const, Extern, Let, If, Else, While, Return, As,
    // punctuation
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Colon, Arrow,          // -> 
    // operators
    Assign, Plus, Minus, Star, Slash, Percent, Amp,
    Eq, Ne, Lt, Le, Gt, Ge,                  // == != < <= > >=
    AndAnd, OrOr, Not,                       // && || !
    Dot,
    // control
    Eof, Error
}