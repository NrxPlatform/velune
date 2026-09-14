#ifndef TOKEN_HPP
#define TOKEN_HPP

#include <cstddef>
#include <string_view>

namespace js::frontend {

enum class TokenKind {
    END_OF_FILE,
    ERROR,

    // literals / names
    NUMBER,
    STRING,
    IDENTIFIER,
    TRUE,
    FALSE,
    NULL_LITERAL,

    // keywords
    LET,
    CONST,
    VAR,
    RETURN,
    THROW,
    TRY,
    CATCH,
    FINALLY,
    FUNCTION,
    THIS,
    NEW,

    IF,
    ELSE,
    WHILE,
    DO,
    FOR,
    SWITCH,
    CASE,
    DEFAULT,

    BREAK,
    CONTINUE,
    DEBUGGER,

    DELETE,
    VOID,
    TYPEOF,
    INSTANCEOF,
    IN,

    ARROW,      // =>

    REGEXP,

    IMPORT,
    EXPORT,
    FROM,

    CLASS,
    EXTENDS,

    ASYNC,
    AWAIT,
    YIELD,

    TEMPLATE, // legacy placeholder
    TEMPLATE_HEAD,
    TEMPLATE_MIDDLE,
    TEMPLATE_TAIL,
    TEMPLATE_NO_SUBSTITUTION,

    // punctuation
    LEFT_PAREN,     // (
    RIGHT_PAREN,    // )
    LEFT_BRACKET,   // [
    RIGHT_BRACKET,  // ]
    LEFT_BRACE,     // {
    RIGHT_BRACE,    // }
    SEMICOLON,      // ;
    COLON,          // :
    COMMA,          // ,
    DOT,            // .
    ELLIPSIS,       // ...
    QUESTION,       // ?
    OPTIONAL_CHAIN, // ?.
    NULLISH,        // ??
    
    // arithmetic
    PLUS,           // +
    MINUS,          // -
    STAR,           // *
    STAR_STAR,      // **
    SLASH,          // /
    PERCENT,        // %

    // update
    PLUS_PLUS,      // ++
    MINUS_MINUS,    // --

    // unary / logical
    BANG,           // !
    TILDE,          // ~

    // relational
    LESS,           // <
    LESS_EQUAL,     // <=
    GREATER,        // >
    GREATER_EQUAL,  // >=

    // equality
    EQUAL_EQUAL,            // ==
    BANG_EQUAL,             // !=
    EQUAL_EQUAL_EQUAL,      // ===
    BANG_EQUAL_EQUAL,       // !==

    // bitwise
    AMPERSAND,      // &
    PIPE,           // |
    CARET,          // ^

    // shifts
    SHIFT_LEFT,             // <<
    SHIFT_RIGHT,            // >>
    SHIFT_RIGHT_UNSIGNED,   // >>>

    // logical
    AND_AND,        // &&
    OR_OR,          // ||

    // assignment
    EQUAL,                  // =
    PLUS_EQUAL,             // +=
    MINUS_EQUAL,            // -=
    STAR_EQUAL,             // *=
    STAR_STAR_EQUAL,        // **=
    SLASH_EQUAL,            // /=
    PERCENT_EQUAL,          // %=
    SHIFT_LEFT_EQUAL,       // <<=
    SHIFT_RIGHT_EQUAL,      // >>=
    SHIFT_RIGHT_UNSIGNED_EQUAL, // >>>=
    AMPERSAND_EQUAL,        // &=
    CARET_EQUAL,            // ^=
    PIPE_EQUAL,             // |=
    AND_AND_EQUAL,          // &&=
    OR_OR_EQUAL,            // ||=
    NULLISH_EQUAL           // ??=
};

struct Token {
    TokenKind kind;

    std::size_t start;
    std::size_t end;

    std::size_t start_line;
    std::size_t start_column;
    std::size_t end_line;
    std::size_t end_column;

    bool line_break_before;

    std::string_view lexeme;
};


} // namespace js::frontend

#endif
