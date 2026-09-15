#pragma once

#include <optional>
#include <cctype>
#include <cstddef>
#include <string_view>

#include <js/frontend/token.hpp>

namespace js::frontend {

struct TokenizerState {
    std::size_t position;
    std::size_t line;
    std::size_t column;
    bool previous_was_cr;

    Token current_token;
    
    bool has_error;
    std::size_t error_start;

    std::optional<Token> previousToken;

};

struct RegExpScanResult {
    Token token;
    std::string_view pattern;
    std::string_view flags;
};

class Tokenizer {
private:
    std::string_view _input;
    std::size_t _position = 0;

    // Source location:
    // line   -> 1-based
    // column -> 0-based
    std::size_t _line = 1;
    std::size_t _column = 0;

    // Used so "\r\n" counts as one line break.
    bool _previous_was_cr = false;

    Token _currentToken{};

    bool _has_error = false;
    std::size_t _error_start = 0;

    std::optional<Token> _previousToken;


private:
    char _peek(std::size_t offset = 0) const {
        if (_position + offset >= _input.length()) {
            return '\0';
        }

        return _input[_position + offset];
    }

    char _advance_char() {
        if (_position >= _input.length()) {
            return '\0';
        }

        char c = _input[_position++];

        if (c == '\r') {
            ++_line;
            _column = 0;
            _previous_was_cr = true;
        }
        else if (c == '\n') {
            // CRLF should count as one newline, not two.
            if (!_previous_was_cr) {
                ++_line;
            }

            _column = 0;
            _previous_was_cr = false;
        }
        else {
            ++_column;
            _previous_was_cr = false;
        }

        return c;
    }

    bool _is_line_terminator(char c) const {
        return c == '\n' || c == '\r';
    }

    Token _make_token(
        TokenKind kind,
        std::size_t start_pos,
        std::size_t start_line,
        std::size_t start_column,
        bool line_break_before
    ) const {
        Token token{};

        token.kind = kind;

        token.start = start_pos;
        token.end = _position;

        token.start_line = start_line;
        token.start_column = start_column;

        token.end_line = _line;
        token.end_column = _column;

        token.line_break_before = line_break_before;

        token.lexeme = _input.substr(
            start_pos,
            _position - start_pos
        );

        return token;
    }

    bool _skip_line_comment() {
        _advance_char(); // '/'
        _advance_char(); // '/'

        while (
            _position < _input.length() &&
            !_is_line_terminator(_peek())
        ) {
            _advance_char();
        }

        // Don't consume the newline here.
        // _skip_ignored() will consume it and record
        // line_break_before correctly.
        return false;
    }

    bool _skip_block_comment() {
        const std::size_t comment_start = _position;

        bool saw_line_break = false;

        _advance_char(); // '/'
        _advance_char(); // '*'

        while (_position < _input.length()) {
            if (_peek() == '*' && _peek(1) == '/') {
                _advance_char(); // '*'
                _advance_char(); // '/'

                return saw_line_break;
            }

            if (_is_line_terminator(_peek())) {
                saw_line_break = true;
            }

            _advance_char();
        }

        // EOF before */
        _has_error = true;
        _error_start = comment_start;

        return saw_line_break;
    }

    bool _skip_ignored() {
        bool saw_line_break = false;

        while (_position < _input.length()) {
            char c = _peek();

            // whitespace
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (_is_line_terminator(c)) {
                    saw_line_break = true;
                }

                _advance_char();
                continue;
            }

            // // comment
            if (c == '/' && _peek(1) == '/') {
                _skip_line_comment();
                continue;
            }

            // /* comment */
            if (c == '/' && _peek(1) == '*') {
                if (_skip_block_comment()) {
                    saw_line_break = true;
                }

                if (_has_error) {
                    return saw_line_break;
                }

                continue;
            }

            break;
        }

        return saw_line_break;
    }

    Token _scan_string(
        char quote_char,
        std::size_t start_pos,
        std::size_t start_line,
        std::size_t start_column,
        bool line_break_before
    ) {
        while (_position < _input.length()) {
            char c = _advance_char();

            // Escaped character.
            if (c == '\\') {
                if (_position < _input.length()) {
                    _advance_char();
                }

                continue;
            }

            if (c == quote_char) {
                return _make_token(
                    TokenKind::STRING,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );
            }

            // Normal JS string literals cannot contain an
            // unescaped line terminator.
            if (_is_line_terminator(c)) {
                return _make_token(
                    TokenKind::ERROR,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );
            }
        }

        // EOF before closing quote.
        return _make_token(
            TokenKind::ERROR,
            start_pos,
            start_line,
            start_column,
            line_break_before
        );
    }


    Token _scan_template_chunk(
        bool initial,
        std::size_t start_pos,
        std::size_t start_line,
        std::size_t start_column,
        bool line_break_before
    ) {
        // For an initial chunk the opening backtick was already consumed.
        // A continuation starts immediately after the interpolation-closing '}'.
        while (_position < _input.length()) {
            const char c = _peek();
            if (c == '\\') {
                _advance_char();
                if (_position < _input.length()) _advance_char();
                continue;
            }
            if (c == '`') {
                _advance_char();
                return _make_token(initial ? TokenKind::TEMPLATE_NO_SUBSTITUTION : TokenKind::TEMPLATE_TAIL,
                                   start_pos, start_line, start_column, line_break_before);
            }
            if (c == '$' && _peek(1) == '{') {
                _advance_char();
                _advance_char();
                return _make_token(initial ? TokenKind::TEMPLATE_HEAD : TokenKind::TEMPLATE_MIDDLE,
                                   start_pos, start_line, start_column, line_break_before);
            }
            _advance_char();
        }
        return _make_token(TokenKind::ERROR, start_pos, start_line, start_column, line_break_before);
    }

    Token _scan_longest(
        char ch,
        std::size_t start_pos,
        std::size_t start_line,
        std::size_t start_column,
        bool line_break_before
    ) {
        auto make = [&](TokenKind kind) {
            return _make_token(kind, start_pos, start_line, start_column, line_break_before);
        };

        switch (ch) {
            case '+':
                if (_peek() == '+') { _advance_char(); return make(TokenKind::PLUS_PLUS); }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::PLUS_EQUAL); }
                return make(TokenKind::PLUS);

            case '-':
                if (_peek() == '-') { _advance_char(); return make(TokenKind::MINUS_MINUS); }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::MINUS_EQUAL); }
                return make(TokenKind::MINUS);

            case '*':
                if (_peek() == '*') {
                    _advance_char();
                    if (_peek() == '=') { _advance_char(); return make(TokenKind::STAR_STAR_EQUAL); }
                    return make(TokenKind::STAR_STAR);
                }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::STAR_EQUAL); }
                return make(TokenKind::STAR);

            case '/':
                if (_peek() == '=') { _advance_char(); return make(TokenKind::SLASH_EQUAL); }
                return make(TokenKind::SLASH);

            case '%':
                if (_peek() == '=') { _advance_char(); return make(TokenKind::PERCENT_EQUAL); }
                return make(TokenKind::PERCENT);

            case '!':
                if (_peek() == '=' && _peek(1) == '=') {
                    _advance_char(); _advance_char(); return make(TokenKind::BANG_EQUAL_EQUAL);
                }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::BANG_EQUAL); }
                return make(TokenKind::BANG);

            case '<':
                if (_peek() == '<') {
                    _advance_char();
                    if (_peek() == '=') { _advance_char(); return make(TokenKind::SHIFT_LEFT_EQUAL); }
                    return make(TokenKind::SHIFT_LEFT);
                }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::LESS_EQUAL); }
                return make(TokenKind::LESS);

            case '>':
                if (_peek() == '>' && _peek(1) == '>') {
                    _advance_char(); _advance_char();
                    if (_peek() == '=') { _advance_char(); return make(TokenKind::SHIFT_RIGHT_UNSIGNED_EQUAL); }
                    return make(TokenKind::SHIFT_RIGHT_UNSIGNED);
                }
                if (_peek() == '>') {
                    _advance_char();
                    if (_peek() == '=') { _advance_char(); return make(TokenKind::SHIFT_RIGHT_EQUAL); }
                    return make(TokenKind::SHIFT_RIGHT);
                }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::GREATER_EQUAL); }
                return make(TokenKind::GREATER);

            case '|':
                if (_peek() == '|') {
                    _advance_char();
                    if (_peek() == '=') { _advance_char(); return make(TokenKind::OR_OR_EQUAL); }
                    return make(TokenKind::OR_OR);
                }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::PIPE_EQUAL); }
                return make(TokenKind::PIPE);

            case '&':
                if (_peek() == '&') {
                    _advance_char();
                    if (_peek() == '=') { _advance_char(); return make(TokenKind::AND_AND_EQUAL); }
                    return make(TokenKind::AND_AND);
                }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::AMPERSAND_EQUAL); }
                return make(TokenKind::AMPERSAND);

            case '^':
                if (_peek() == '=') { _advance_char(); return make(TokenKind::CARET_EQUAL); }
                return make(TokenKind::CARET);

            case '?':
                if (_peek() == '?') {
                    _advance_char();
                    if (_peek() == '=') { _advance_char(); return make(TokenKind::NULLISH_EQUAL); }
                    return make(TokenKind::NULLISH);
                }
                if (_peek() == '.') { _advance_char(); return make(TokenKind::OPTIONAL_CHAIN); }
                return make(TokenKind::QUESTION);

            case '=':
                if (_peek() == '>') { _advance_char(); return make(TokenKind::ARROW); }
                if (_peek() == '=' && _peek(1) == '=') {
                    _advance_char(); _advance_char(); return make(TokenKind::EQUAL_EQUAL_EQUAL);
                }
                if (_peek() == '=') { _advance_char(); return make(TokenKind::EQUAL_EQUAL); }
                return make(TokenKind::EQUAL);
        }

        return make(TokenKind::ERROR);
    }

    Token _scan_identifier(
        std::size_t start_pos,
        std::size_t start_line,
        std::size_t start_column,
        bool line_break_before
    ) {
        while (
            std::isalnum(static_cast<unsigned char>(_peek())) ||
            _peek() == '_' ||
            _peek() == '$'
        ) {
            _advance_char();
        }

        std::string_view value = _input.substr(
            start_pos,
            _position - start_pos
        );

        TokenKind kind = TokenKind::IDENTIFIER;

        if      (value == "let")      kind = TokenKind::LET;
        else if (value == "const")    kind = TokenKind::CONST;
        else if (value == "var")      kind = TokenKind::VAR;
        else if (value == "return")   kind = TokenKind::RETURN;
        else if (value == "throw")    kind = TokenKind::THROW;
        else if (value == "try")      kind = TokenKind::TRY;
        else if (value == "catch")    kind = TokenKind::CATCH;
        else if (value == "finally")  kind = TokenKind::FINALLY;
        else if (value == "function") kind = TokenKind::FUNCTION;
        else if (value == "this")     kind = TokenKind::THIS;
        else if (value == "true")     kind = TokenKind::TRUE;
        else if (value == "false")    kind = TokenKind::FALSE;
        else if (value == "null")     kind = TokenKind::NULL_LITERAL;
        else if (value == "new")      kind = TokenKind::NEW;

        else if (value == "if")       kind = TokenKind::IF;
        else if (value == "else")     kind = TokenKind::ELSE;
        else if (value == "while")    kind = TokenKind::WHILE;
        else if (value == "do")       kind = TokenKind::DO;
        else if (value == "for")      kind = TokenKind::FOR;
        else if (value == "switch")   kind = TokenKind::SWITCH;
        else if (value == "case")     kind = TokenKind::CASE;
        else if (value == "default")  kind = TokenKind::DEFAULT;

        else if (value == "break")    kind = TokenKind::BREAK;
        else if (value == "continue") kind = TokenKind::CONTINUE;
        else if (value == "debugger") kind = TokenKind::DEBUGGER;

        else if (value == "delete")   kind = TokenKind::DELETE;
        else if (value == "void")     kind = TokenKind::VOID;
        else if (value == "typeof")   kind = TokenKind::TYPEOF;
        else if (value == "instanceof") kind = TokenKind::INSTANCEOF;
        else if (value == "in")       kind = TokenKind::IN;

        else if (value == "import")   kind = TokenKind::IMPORT;
        else if (value == "export")   kind = TokenKind::EXPORT;
        else if (value == "from")     kind = TokenKind::FROM;

        else if (value == "class")    kind = TokenKind::CLASS;
        else if (value == "extends")  kind = TokenKind::EXTENDS;

        else if (value == "async")    kind = TokenKind::ASYNC;
        else if (value == "await")    kind = TokenKind::AWAIT;
        else if (value == "yield")    kind = TokenKind::YIELD;

        return _make_token(
            kind,
            start_pos,
            start_line,
            start_column,
            line_break_before
        );
    }

    Token _scan_number(
        std::size_t start_pos,
        std::size_t start_line,
        std::size_t start_column,
        bool line_break_before
    ) {
        while (std::isdigit(static_cast<unsigned char>(_peek()))) {
            _advance_char();
        }

        // DecimalLiteral subset used by the current Number domain:
        //   DecimalIntegerLiteral . DecimalDigits? ExponentPart?
        //   DecimalIntegerLiteral ExponentPart?
        // Leading-dot literals are handled by the '.' token path separately.
        if (_peek() == '.') {
            _advance_char();
            while (std::isdigit(static_cast<unsigned char>(_peek()))) {
                _advance_char();
            }
        }

        if (_peek() == 'e' || _peek() == 'E') {
            const std::size_t exponent_start = _position;
            _advance_char();
            if (_peek() == '+' || _peek() == '-') _advance_char();

            if (std::isdigit(static_cast<unsigned char>(_peek()))) {
                while (std::isdigit(static_cast<unsigned char>(_peek()))) {
                    _advance_char();
                }
            } else {
                // Do not absorb an invalid exponent marker into the numeric token;
                // leaving it for the normal token stream produces the syntax error.
                const std::size_t consumed = _position - exponent_start;
                _position = exponent_start;
                _column -= consumed;
            }
        }

        return _make_token(
            TokenKind::NUMBER,
            start_pos,
            start_line,
            start_column,
            line_break_before
        );
    }

    Token _scan_next_token() {
        _has_error = false;

        bool line_break_before = _skip_ignored();

        if (_has_error) {
            // Location here starts at the position where the
            // unterminated block comment began.
            //
            // We don't currently retain its exact starting
            // line/column, so ERROR location is approximate.
            Token token{};

            token.kind = TokenKind::ERROR;
            token.start = _error_start;
            token.end = _position;
            token.end_line = _line;
            token.end_column = _column;
            token.line_break_before = line_break_before;
            token.lexeme = _input.substr(
                _error_start,
                _position - _error_start
            );

            return token;
        }

        const std::size_t start_pos = _position;
        const std::size_t start_line = _line;
        const std::size_t start_column = _column;

        if (_position >= _input.length()) {
            return _make_token(
                TokenKind::END_OF_FILE,
                start_pos,
                start_line,
                start_column,
                line_break_before
            );
        }

        char c = _advance_char();

        switch (c) {
            case '`':
                return _scan_template_chunk(true, start_pos, start_line, start_column, line_break_before);

            // strings
            case '"':
            case '\'':
                return _scan_string(
                    c,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            // punctuation
            case '(':
                return _make_token(
                    TokenKind::LEFT_PAREN,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case ')':
                return _make_token(
                    TokenKind::RIGHT_PAREN,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case '[':
                return _make_token(
                    TokenKind::LEFT_BRACKET,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case ']':
                return _make_token(
                    TokenKind::RIGHT_BRACKET,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case '{':
                return _make_token(
                    TokenKind::LEFT_BRACE,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case '}':
                return _make_token(
                    TokenKind::RIGHT_BRACE,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case ';':
                return _make_token(
                    TokenKind::SEMICOLON,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case ',':
                return _make_token(
                    TokenKind::COMMA,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case '.':
                if (_peek() == '.' && _peek(1) == '.') {
                    _advance_char();
                    _advance_char();
                    return _make_token(TokenKind::ELLIPSIS, start_pos, start_line, start_column, line_break_before);
                }
                return _make_token(
                    TokenKind::DOT,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case ':':
                return _make_token(
                    TokenKind::COLON,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case '?':
                return _scan_longest(
                    c, start_pos, start_line, start_column, line_break_before
                );

            // arithmetic / update
            case '+':
            case '-':
                return _scan_longest(
                    c,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case '*':
            case '/':
            case '%':
                // Plain '/' remains SLASH so the parser can still reinterpret it
                // as a RegExp literal at expression-start. '/=' is tokenized here.
                return _scan_longest(
                    c,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            // unary
            case '!':
                return _scan_longest(
                    c,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case '~':
                return _make_token(
                    TokenKind::TILDE,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            // relational / shift
            case '<':
            case '>':
                return _scan_longest(
                    c,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            // bitwise / logical
            case '&':
            case '|':
                return _scan_longest(
                    c,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );

            case '^':
                return _scan_longest(
                    c, start_pos, start_line, start_column, line_break_before
                );

            // equality / assignment / arrow
            case '=':
                return _scan_longest(
                    c,
                    start_pos,
                    start_line,
                    start_column,
                    line_break_before
                );
        }

        if (
            std::isalpha(static_cast<unsigned char>(c)) ||
            c == '_' ||
            c == '$'
        ) {
            return _scan_identifier(
                start_pos,
                start_line,
                start_column,
                line_break_before
            );
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            return _scan_number(
                start_pos,
                start_line,
                start_column,
                line_break_before
            );
        }

        return _make_token(
            TokenKind::ERROR,
            start_pos,
            start_line,
            start_column,
            line_break_before
        );
    }

public:
    explicit Tokenizer(std::string_view input)
        : _input(input)
    {
        _currentToken = _scan_next_token();
    }

    std::size_t input_length() const {
        return _input.length();
    }

    Token current() const {
        return _currentToken;
    }

    void advance() {
        _previousToken = _currentToken;
        _currentToken = _scan_next_token();
    }

    void advance_template_continuation() {
        if (_currentToken.kind != TokenKind::RIGHT_BRACE) {
            throw std::runtime_error("template continuation requires interpolation-closing brace");
        }
        _previousToken = _currentToken;
        const std::size_t start_pos = _position;
        const std::size_t start_line = _line;
        const std::size_t start_column = _column;
        _currentToken = _scan_template_chunk(false, start_pos, start_line, start_column, false);
    }

    const std::optional<Token>& previous() const {
        return _previousToken;
    }

    TokenizerState checkpoint() const {
        return TokenizerState{
            _position,
            _line,
            _column,
            _previous_was_cr,
            _currentToken,
            _has_error,
            _error_start,
            _previousToken
        };
    }

    void restore(const TokenizerState& state) {
        _position = state.position;
        _line = state.line;
        _column = state.column;
        _previous_was_cr = state.previous_was_cr;
        _currentToken = state.current_token;
        _has_error = state.has_error;
        _error_start = state.error_start;
        _previousToken = state.previousToken;
    }

    RegExpScanResult scan_regexp(){

        if (_currentToken.kind != TokenKind::SLASH){
            throw std::runtime_error(
                "scan_regexp called without slash token"
            );
        }


        Token opening_slash = _currentToken;

        std::size_t pattern_start = _position;
        bool escaped = false;
        bool in_character_class = false;

        while (_position < _input.size()) {
            char c = _peek();

            // Regex literal cannot cross a raw newline.
            if (c == '\n' || c == '\r') {
                throw std::runtime_error(
                    "Unterminated regular expression"
                );
            }

            if (escaped) {
                _advance_char();
                escaped = false;
                continue;
            }

            if (c == '\\') {
                escaped = true;
                _advance_char();
                continue;
            }

            if (c == '[') {
                in_character_class = true;
                _advance_char();
                continue;
            }

            if (c == ']' && in_character_class) {
                in_character_class = false;
                _advance_char();
                continue;
            }

            if (c == '/' && !in_character_class) {
                break;
            }

            _advance_char();
        }

        if (_position >= _input.size()) {
            throw std::runtime_error(
                "Unterminated regular expression"
            );
        }

        std::size_t pattern_end = _position;

        _advance_char();
        
        std::size_t flags_start = _position;

        while (_position < _input.size() && std::isalpha(static_cast<unsigned char>(_peek()))) {
            _advance_char();
        }

        std::size_t regex_end = _position;

        auto pattern = _input.substr(
            pattern_start,
            pattern_end - pattern_start
        );

        auto flags = _input.substr(
            flags_start,
            regex_end - flags_start
        );

        Token regex_token{
            TokenKind::REGEXP,
            opening_slash.start,
            regex_end,

            opening_slash.start_line,
            opening_slash.start_column,

            _line,
            _column,

            opening_slash.line_break_before,

            _input.substr(
                opening_slash.start,
                regex_end - opening_slash.start
            )
        };

        _currentToken = regex_token;

        return {
            _currentToken,
            pattern,
            flags
        };

    }

    

    bool is_at_end() const {
        return _currentToken.kind ==
               TokenKind::END_OF_FILE;
    }
};

} // namespace js::frontend

