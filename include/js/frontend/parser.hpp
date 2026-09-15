#ifndef PARSER_HPP
#define PARSER_HPP

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <js/frontend/tokenizer.hpp>
#include <js/frontend/ast.hpp>

namespace js::frontend {

struct GrammarContext {
    bool allow_yield = false;
    bool allow_await = false;
    bool allow_in = true;
    bool allow_return = false;
    bool strict = false;
};

class GrammarContextGuard {
private:
    GrammarContext& _context;
    GrammarContext _previous;

public:
    GrammarContextGuard(GrammarContext& context, GrammarContext next)
        : _context(context), _previous(context)
    {
        _context = next;
    }

    ~GrammarContextGuard() {
        _context = _previous;
    }
};

class FunctionContextGuard {
private:
    std::size_t& _function_depth;
    std::size_t& _loop_depth;
    std::size_t& _breakable_depth;
    std::size_t& _async_function_depth;
    std::size_t& _generator_function_depth;

    std::size_t _previous_loop_depth;
    std::size_t _previous_breakable_depth;

    bool _is_async;
    bool _is_generator;

public:
    FunctionContextGuard(
        std::size_t& function_depth,
        std::size_t& loop_depth,
        std::size_t& breakable_depth,
        std::size_t& async_function_depth,
        std::size_t& generator_function_depth,
        bool is_async,
        bool is_generator
    )
        : _function_depth(function_depth),
          _loop_depth(loop_depth),
          _breakable_depth(breakable_depth),
          _async_function_depth(async_function_depth),
          _generator_function_depth(generator_function_depth),
          _previous_loop_depth(loop_depth),
          _previous_breakable_depth(breakable_depth),
          _is_async(is_async),
          _is_generator(is_generator)
    {
        ++_function_depth;
        _loop_depth = 0;
        _breakable_depth = 0;

        if (_is_async)
            ++_async_function_depth;

        if (_is_generator)
            ++_generator_function_depth;
    }

    ~FunctionContextGuard() {
        if (_is_generator)
            --_generator_function_depth;

        if (_is_async)
            --_async_function_depth;

        _loop_depth = _previous_loop_depth;
        _breakable_depth = _previous_breakable_depth;
        --_function_depth;
    }
};
class LoopContextGuard {
private:
    std::size_t& _loop_depth;

public:
    explicit LoopContextGuard(std::size_t& loop_depth)
        : _loop_depth(loop_depth)
    {
        ++_loop_depth;
    }

    ~LoopContextGuard() {
        --_loop_depth;
    }
};

class BreakableContextGuard {
private:
    std::size_t& _depth;
public:
    explicit BreakableContextGuard(std::size_t& depth) : _depth(depth) { ++_depth; }
    ~BreakableContextGuard() { --_depth; }
};

class ParseException final : public std::runtime_error {
public:
    ParseException(std::string message, const Token& token)
        : std::runtime_error(std::move(message)),
          offset(token.start),
          line(token.start_line),
          column(token.start_column) {}

    std::size_t offset;
    std::size_t line;
    std::size_t column;
};

class Parser {
private:
    Tokenizer _tokenizer;

    std::size_t _function_depth = 0;
    std::size_t _loop_depth = 0;
    std::size_t _breakable_depth = 0;
    std::size_t _async_function_depth = 0;
    std::size_t _generator_function_depth = 0;

    bool _is_async = false;
    bool _is_generator = false;

    GrammarContext _grammar_context{};

    struct ActiveLabel {
        std::string name;
        bool iteration;
        std::size_t function_depth;
    };
    std::vector<ActiveLabel> _active_labels;

    Token _peek() const {
        return _tokenizer.current();
    }

    [[noreturn]] void _fail(std::string message) const {
        throw ParseException(std::move(message), _peek());
    }

    Token _consume(TokenKind expected_kind){
        Token token = _peek();
        if (token.kind != expected_kind){
            _fail("Unexpected token in input");
        }
        _tokenizer.advance();
        return token;
    }

    std::size_t _consume_semicolon_or_insert() {
        if (_peek().kind == TokenKind::SEMICOLON) {
            Token semicolon = _consume(TokenKind::SEMICOLON);
            return semicolon.end;
        }

        if (
            _peek().kind == TokenKind::RIGHT_BRACE ||
            _peek().kind == TokenKind::END_OF_FILE ||
            _peek().line_break_before
        ) {
            const auto& previous = _tokenizer.previous();

            if (!previous) {
                _fail(
                    "Internal parser error: ASI without previous token"
                );
            }

            return previous->end;
        }

        _fail(
            "Expected semicolon"
        );
    }
    

    enum class AssignmentTargetKind {
        INVALID,
        SIMPLE,
        PATTERN
    };

    AssignmentTargetKind _classify_assignment_target(const ASTNode* node) const {
        if (node == nullptr) return AssignmentTargetKind::INVALID;

        switch (node->type) {
            case ASTNodeType::IDENTIFIER:
            case ASTNodeType::MEMBER_EXPR:
                return AssignmentTargetKind::SIMPLE;
            case ASTNodeType::ARRAY_PATTERN:
            case ASTNodeType::OBJECT_PATTERN:
                return AssignmentTargetKind::PATTERN;
            default:
                return AssignmentTargetKind::INVALID;
        }
    }

    bool _is_simple_assignment_target(const ASTNode* node) const {
        return _classify_assignment_target(node) == AssignmentTargetKind::SIMPLE;
    }

    bool _is_identifier_name_token(TokenKind kind) const {
        switch (kind) {
            case TokenKind::IDENTIFIER:
            case TokenKind::LET:
            case TokenKind::CONST:
            case TokenKind::VAR:
            case TokenKind::RETURN:
            case TokenKind::THROW:
            case TokenKind::TRY:
            case TokenKind::CATCH:
            case TokenKind::FINALLY:
            case TokenKind::FUNCTION:
            case TokenKind::THIS:
            case TokenKind::TRUE:
            case TokenKind::FALSE:
            case TokenKind::NULL_LITERAL:
            case TokenKind::NEW:
            case TokenKind::IF:
            case TokenKind::ELSE:
            case TokenKind::WHILE:
            case TokenKind::DO:
            case TokenKind::FOR:
            case TokenKind::SWITCH:
            case TokenKind::CASE:
            case TokenKind::DEFAULT:
            case TokenKind::BREAK:
            case TokenKind::CONTINUE:
            case TokenKind::DEBUGGER:
            case TokenKind::DELETE:
            case TokenKind::VOID:
            case TokenKind::TYPEOF:
            case TokenKind::INSTANCEOF:
            case TokenKind::IN:
            case TokenKind::IMPORT:
            case TokenKind::EXPORT:
            case TokenKind::FROM:
            case TokenKind::CLASS:
            case TokenKind::EXTENDS:
            case TokenKind::ASYNC:
            case TokenKind::AWAIT:
            case TokenKind::YIELD:
                return true;
            default:
                return false;
        }
    }

    using ParseLevel = std::unique_ptr<ASTNode> (Parser::*)();

    bool _matches(TokenKind kind, std::initializer_list<TokenKind> kinds) const {
        for (const auto candidate : kinds) {
            if (kind == candidate) return true;
        }
        return false;
    }

    std::unique_ptr<ASTNode> _parse_left_associative(
        ParseLevel next,
        std::initializer_list<TokenKind> operators,
        bool logical = false
    ) {
        auto left = (this->*next)();

        while (_matches(_peek().kind, operators)) {
            Token op_token = _peek();
            _tokenizer.advance();
            auto right = (this->*next)();

            if (logical) {
                left = std::make_unique<LogicalExprNode>(
                    op_token.kind, std::move(left), std::move(right));
            } else {
                left = std::make_unique<BinaryExprNode>(
                    op_token.kind, std::move(left), std::move(right));
            }
        }

        return left;
    }

    void _validate_variable_declarator(VariableKind kind, const VariableDeclaratorNode* declarator) const {
        const bool destructuring = declarator->id->type == ASTNodeType::ARRAY_PATTERN ||
                                   declarator->id->type == ASTNodeType::OBJECT_PATTERN;
        if (destructuring && declarator->init == nullptr) {
            _fail("Destructuring declaration requires initializer");
        }
        if (kind == VariableKind::CONST && declarator->init == nullptr) {
            _fail("Const declaration requires initializer");
        }
    }


    std::vector<std::unique_ptr<ASTNode>> _parse_arguments(){
        std::vector<std::unique_ptr<ASTNode>> args;

        if (_peek().kind == TokenKind::RIGHT_PAREN) {
            return args;
        }

        while (true){
            if (_peek().kind == TokenKind::ELLIPSIS) {
                Token spread = _consume(TokenKind::ELLIPSIS);
                args.push_back(std::make_unique<SpreadElementNode>(spread.start, _parse_assignment()));
            } else {
                args.push_back(_parse_assignment());
            }

            if (_peek().kind != TokenKind::COMMA){
                break;
            }

            _consume(TokenKind::COMMA);
            if (_peek().kind == TokenKind::RIGHT_PAREN) break;
        }

        return args;
        
    }

    std::unique_ptr<ASTNode> _parse_pattern(bool binding_context, bool allow_default = true) {
        std::unique_ptr<ASTNode> target;
        if (_peek().kind == TokenKind::LEFT_BRACKET) {
            Token left = _consume(TokenKind::LEFT_BRACKET);
            std::vector<std::unique_ptr<ASTNode>> elements;
            while (_peek().kind != TokenKind::RIGHT_BRACKET) {
                if (_peek().kind == TokenKind::COMMA) {
                    _tokenizer.advance();
                    elements.push_back(nullptr);
                    continue;
                }
                if (_peek().kind == TokenKind::ELLIPSIS) {
                    Token rest = _consume(TokenKind::ELLIPSIS);
                    auto argument = _parse_pattern(binding_context, false);
                    elements.push_back(std::make_unique<RestElementNode>(rest.start, std::move(argument)));
                    if (_peek().kind == TokenKind::COMMA) _fail("Rest element must be last");
                    break;
                }
                elements.push_back(_parse_pattern(binding_context, true));
                if (_peek().kind != TokenKind::COMMA) break;
                _tokenizer.advance();
                if (_peek().kind == TokenKind::RIGHT_BRACKET) break;
            }
            Token right = _consume(TokenKind::RIGHT_BRACKET);
            target = std::make_unique<ArrayPatternNode>(left, right, std::move(elements));
        } else if (_peek().kind == TokenKind::LEFT_BRACE) {
            Token left = _consume(TokenKind::LEFT_BRACE);
            std::vector<std::unique_ptr<ObjectPatternPropertyNode>> properties;
            std::unique_ptr<RestElementNode> rest_element;
            while (_peek().kind != TokenKind::RIGHT_BRACE) {
                if (_peek().kind == TokenKind::ELLIPSIS) {
                    Token rest = _consume(TokenKind::ELLIPSIS);
                    auto argument = _parse_pattern(binding_context, false);
                    rest_element = std::make_unique<RestElementNode>(rest.start, std::move(argument));
                    if (_peek().kind == TokenKind::COMMA) _fail("Object rest element must be last");
                    break;
                }
                bool computed = false;
                std::unique_ptr<ASTNode> key;
                if (_peek().kind == TokenKind::LEFT_BRACKET) {
                    computed = true;
                    _consume(TokenKind::LEFT_BRACKET);
                    key = _parse_expression();
                    _consume(TokenKind::RIGHT_BRACKET);
                } else {
                    Token key_token = _peek();
                    if (key_token.kind != TokenKind::IDENTIFIER && key_token.kind != TokenKind::STRING && key_token.kind != TokenKind::NUMBER)
                        _fail("Expected object pattern property name");
                    _tokenizer.advance();
                    if (key_token.kind == TokenKind::IDENTIFIER) key = std::make_unique<IdentifierNode>(key_token);
                    else if (key_token.kind == TokenKind::STRING) key = std::make_unique<StringLiteralNode>(key_token);
                    else key = std::make_unique<NumberLiteralNode>(key_token);
                }
                std::unique_ptr<ASTNode> value;
                if (_peek().kind == TokenKind::COLON) {
                    _tokenizer.advance();
                    value = _parse_pattern(binding_context, true);
                } else {
                    if (computed || key->type != ASTNodeType::IDENTIFIER)
                        _fail("Object pattern shorthand requires identifier key");
                    const auto& identifier = static_cast<const IdentifierNode&>(*key);
                    value = std::make_unique<IdentifierNode>(identifier.name, identifier.start, identifier.end);
                    if (allow_default && _peek().kind == TokenKind::EQUAL) {
                        _tokenizer.advance();
                        value = std::make_unique<AssignmentPatternNode>(std::move(value), _parse_assignment());
                    }
                }
                properties.push_back(std::make_unique<ObjectPatternPropertyNode>(std::move(key), std::move(value), computed));
                if (_peek().kind != TokenKind::COMMA) break;
                _tokenizer.advance();
                if (_peek().kind == TokenKind::RIGHT_BRACE) break;
            }
            Token right = _consume(TokenKind::RIGHT_BRACE);
            target = std::make_unique<ObjectPatternNode>(left, right, std::move(properties), std::move(rest_element));
        } else {
            if (!_is_identifier_reference(_peek().kind)) _fail("Expected binding or assignment target in pattern");
            if (binding_context) {
                target = _consume_identifier_reference();
            } else {
                target = _parse_member_call();
                if (!_is_simple_assignment_target(target.get())) _fail("Invalid assignment target in destructuring pattern");
            }
        }
        if (allow_default && _peek().kind == TokenKind::EQUAL) {
            _tokenizer.advance();
            target = std::make_unique<AssignmentPatternNode>(std::move(target), _parse_assignment());
        }
        return target;
    }

    struct ParsedParameters {
        std::vector<std::unique_ptr<ASTNode>> params;
        std::vector<std::unique_ptr<ASTNode>> defaults;
        std::optional<std::size_t> rest_parameter;
    };

    ParsedParameters _parse_parameters(){
        ParsedParameters result;
        if (_peek().kind == TokenKind::RIGHT_PAREN) return result;

        while (true) {
            const bool is_rest = _peek().kind == TokenKind::ELLIPSIS;
            if (is_rest) {
                Token rest = _consume(TokenKind::ELLIPSIS);
                (void)rest;
                if (result.rest_parameter) _fail("Only one rest parameter is permitted");
                result.rest_parameter = result.params.size();
            }

            auto parameter = _parse_pattern(true, false);
            result.params.push_back(std::move(parameter));
            result.defaults.push_back(nullptr);

            if (is_rest) {
                if (_peek().kind == TokenKind::EQUAL) _fail("Rest parameter may not have a default initializer");
                if (_peek().kind != TokenKind::RIGHT_PAREN) _fail("Rest parameter must be last");
                break;
            }

            if (_peek().kind == TokenKind::EQUAL) {
                _tokenizer.advance();
                result.defaults.back() = _parse_assignment();
            }

            if (_peek().kind != TokenKind::COMMA) break;
            _tokenizer.advance();
            if (_peek().kind == TokenKind::RIGHT_PAREN) break;
        }
        return result;
    }

    std::unique_ptr<VariableDeclaratorNode> _parse_variable_declarator(){
        auto id = _parse_pattern(true, false);

        std::unique_ptr<ASTNode> init;

        if (_peek().kind == TokenKind::EQUAL){
            _tokenizer.advance();
            init = _parse_assignment();
        }

        return std::make_unique<VariableDeclaratorNode>(
            std::move(id),
            std::move(init)
        );
    }

    std::unique_ptr<VariableDeclarationNode> _parse_variable_declaration(){
        Token keyword = _peek();

        VariableKind kind;

        switch (keyword.kind) {
            case TokenKind::LET:
                kind = VariableKind::LET;
                break;

            case TokenKind::CONST:
                kind = VariableKind::CONST;
                break;

            case TokenKind::VAR:
                kind = VariableKind::VAR;
                break;

            default:
                _fail(
                    "Expected variable declaration keyword"
                );
        }

        _tokenizer.advance();

        std::vector<std::unique_ptr<VariableDeclaratorNode>> declarations;
        
        auto first_declarator = _parse_variable_declarator();
        
        _validate_variable_declarator(kind, first_declarator.get());

        declarations.push_back(std::move(first_declarator));

        while (_peek().kind == TokenKind::COMMA){
            _tokenizer.advance();

            auto next_declarator = _parse_variable_declarator();

            _validate_variable_declarator(kind, next_declarator.get());

            declarations.push_back(std::move(next_declarator));
        }

        // Token semicolon = _consume(TokenKind::SEMICOLON);
        std::size_t end = _consume_semicolon_or_insert();
        
        return std::make_unique<VariableDeclarationNode>(keyword,
            kind, std::move(declarations), end);
    }

    std::unique_ptr<FunctionDeclarationNode> _parse_function_declaration(){
        Token function_token = _consume(TokenKind::FUNCTION);
        const bool is_generator = _peek().kind == TokenKind::STAR;
        if (is_generator) _consume(TokenKind::STAR);

        Token function_name = _consume(TokenKind::IDENTIFIER);

        auto id = std::make_unique<IdentifierNode>(function_name);

        _consume(TokenKind::LEFT_PAREN);

        auto parsed_params = _parse_parameters();

        _consume(TokenKind::RIGHT_PAREN);

        FunctionContextGuard guard(_function_depth, _loop_depth, _breakable_depth, _async_function_depth,
            _generator_function_depth, false, is_generator);
        GrammarContext function_context = _grammar_context;
        function_context.allow_return = true;
        function_context.allow_yield = is_generator;
        function_context.allow_await = false;
        GrammarContextGuard grammar_guard(_grammar_context, function_context);

        auto body = _parse_block_statement();

        auto node = std::make_unique<FunctionDeclarationNode>(
            function_token,
            std::move(id),
            std::move(parsed_params.params),
            std::move(body),
            false,
            is_generator
        );
        node->param_defaults = std::move(parsed_params.defaults);
        node->rest_parameter = parsed_params.rest_parameter;
        return node;
    }

    std::unique_ptr<FunctionExpressionNode> _parse_function_expression(){
        Token function_token = _consume(TokenKind::FUNCTION);
        const bool is_generator = _peek().kind == TokenKind::STAR;
        if (is_generator) _consume(TokenKind::STAR);

        std::unique_ptr<IdentifierNode> id;
        if (_peek().kind == TokenKind::IDENTIFIER) {
            Token function_name = _consume(TokenKind::IDENTIFIER);
            id = std::make_unique<IdentifierNode>(function_name);
        }

        _consume(TokenKind::LEFT_PAREN);
        auto parsed_params = _parse_parameters();
        _consume(TokenKind::RIGHT_PAREN);

        FunctionContextGuard guard(_function_depth, _loop_depth, _breakable_depth, _async_function_depth,
            _generator_function_depth, false, is_generator);
        GrammarContext function_context = _grammar_context;
        function_context.allow_return = true;
        function_context.allow_yield = is_generator;
        function_context.allow_await = false;
        GrammarContextGuard grammar_guard(_grammar_context, function_context);

        auto body = _parse_block_statement();
        auto node = std::make_unique<FunctionExpressionNode>(
            function_token, std::move(id), std::move(parsed_params.params), std::move(body), false, is_generator);
        node->param_defaults = std::move(parsed_params.defaults);
        node->rest_parameter = parsed_params.rest_parameter;
        return node;
    }

    std::unique_ptr<ArrayExprNode> _parse_array_expression() {
        Token left_bracket = _consume(TokenKind::LEFT_BRACKET);
        std::vector<std::unique_ptr<ASTNode>> elements;

        while (_peek().kind != TokenKind::RIGHT_BRACKET) {
            if (_peek().kind == TokenKind::COMMA) {
                elements.push_back(nullptr); // Array elision / hole.
                _tokenizer.advance();
                continue;
            }
            if (_peek().kind == TokenKind::ELLIPSIS) {
                Token spread = _consume(TokenKind::ELLIPSIS);
                elements.push_back(std::make_unique<SpreadElementNode>(spread.start, _parse_assignment()));
            } else {
                elements.push_back(_parse_assignment());
            }
            if (_peek().kind != TokenKind::COMMA) break;
            _tokenizer.advance();
            if (_peek().kind == TokenKind::RIGHT_BRACKET) break;
        }

        Token right_bracket = _consume(TokenKind::RIGHT_BRACKET);
        return std::make_unique<ArrayExprNode>(left_bracket, right_bracket, std::move(elements));
    }

    std::unique_ptr<ASTNode> _parse_object_property() {
        if (_peek().kind == TokenKind::ELLIPSIS) {
            Token spread = _consume(TokenKind::ELLIPSIS);
            return std::make_unique<SpreadElementNode>(spread.start, _parse_assignment());
        }

        auto parse_property_name = [this](bool& computed) -> std::unique_ptr<ASTNode> {
            computed = false;
            if (_peek().kind == TokenKind::LEFT_BRACKET) {
                computed = true;
                _tokenizer.advance();
                auto key = _parse_expression();
                _consume(TokenKind::RIGHT_BRACKET);
                return key;
            }
            if (_is_identifier_name_token(_peek().kind)) {
                Token key_token = _peek();
                _tokenizer.advance();
                return std::make_unique<IdentifierNode>(key_token);
            }
            if (_peek().kind == TokenKind::STRING) {
                Token key_token = _peek(); _tokenizer.advance();
                return std::make_unique<StringLiteralNode>(key_token);
            }
            if (_peek().kind == TokenKind::NUMBER) {
                Token key_token = _peek(); _tokenizer.advance();
                return std::make_unique<NumberLiteralNode>(key_token);
            }
            _fail("Expected object literal property name");
        };

        bool computed = false;
        auto key = parse_property_name(computed);

        // `get` is contextual in an object literal.  It remains an ordinary
        // key in `{ get: v }` and `{ get }`, but introduces an accessor when a
        // second PropertyName is followed by an empty parameter list.
        if (!computed && key->type == ASTNodeType::IDENTIFIER &&
            static_cast<const IdentifierNode&>(*key).name == "get" &&
            _peek().kind != TokenKind::COLON && _peek().kind != TokenKind::COMMA &&
            _peek().kind != TokenKind::RIGHT_BRACE && _peek().kind != TokenKind::LEFT_PAREN) {
            Token get_token{};
            get_token.kind = TokenKind::IDENTIFIER;
            get_token.start = key->start; get_token.end = key->end;
            get_token.lexeme = "get";
            bool accessor_computed = false;
            auto accessor_key = parse_property_name(accessor_computed);
            _consume(TokenKind::LEFT_PAREN);
            if (_peek().kind != TokenKind::RIGHT_PAREN) _fail("Getter must not have parameters");
            _consume(TokenKind::RIGHT_PAREN);

            FunctionContextGuard guard(_function_depth, _loop_depth, _breakable_depth, _async_function_depth,
                _generator_function_depth, false, false);
            GrammarContext function_context = _grammar_context;
            function_context.allow_return = true;
            function_context.allow_yield = false;
            function_context.allow_await = false;
            GrammarContextGuard grammar_guard(_grammar_context, function_context);
            auto body = _parse_block_statement();
            auto getter = std::make_unique<FunctionExpressionNode>(
                get_token, nullptr, std::vector<std::unique_ptr<ASTNode>>{}, std::move(body), false, false);
            return std::make_unique<PropertyNode>(std::move(accessor_key), std::move(getter), false,
                accessor_computed, PropertyKind::Getter);
        }

        if (_peek().kind == TokenKind::COLON) {
            _tokenizer.advance();
            auto value = _parse_assignment();
            return std::make_unique<PropertyNode>(std::move(key), std::move(value), false, computed);
        }

        if (!computed && key->type == ASTNodeType::IDENTIFIER) {
            const auto* identifier = static_cast<const IdentifierNode*>(key.get());
            Token synthetic{};
            synthetic.kind = TokenKind::IDENTIFIER;
            synthetic.start = identifier->start; synthetic.end = identifier->end;
            synthetic.lexeme = identifier->name;
            auto value = std::make_unique<IdentifierNode>(synthetic);
            return std::make_unique<PropertyNode>(std::move(key), std::move(value), true, false);
        }
        _fail("Expected ':' after object literal property name");
    }

    std::unique_ptr<ObjectExprNode> _parse_object_expression(){
        Token left_brace = _consume(TokenKind::LEFT_BRACE);
        std::vector<std::unique_ptr<ASTNode>> properties;

        while (_peek().kind != TokenKind::RIGHT_BRACE) {
            properties.push_back(_parse_object_property());
            if (_peek().kind != TokenKind::COMMA) break;
            _tokenizer.advance();
            if (_peek().kind == TokenKind::RIGHT_BRACE) break;
        }

        Token right_brace = _consume(TokenKind::RIGHT_BRACE);
        return std::make_unique<ObjectExprNode>(left_brace, right_brace, std::move(properties));
    }

    static std::string_view _template_chunk_text(const Token& token) {
        auto text = token.lexeme;
        if (token.kind == TokenKind::TEMPLATE_NO_SUBSTITUTION) {
            if (text.size() >= 2) return text.substr(1, text.size() - 2);
        } else if (token.kind == TokenKind::TEMPLATE_HEAD) {
            if (text.size() >= 3) return text.substr(1, text.size() - 3);
        } else if (token.kind == TokenKind::TEMPLATE_MIDDLE) {
            if (text.size() >= 2) return text.substr(0, text.size() - 2);
        } else if (token.kind == TokenKind::TEMPLATE_TAIL) {
            if (!text.empty()) return text.substr(0, text.size() - 1);
        }
        return {};
    }

    static std::string _cook_template_text(std::string_view raw) {
        std::string cooked;
        cooked.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            char c = raw[i];
            if (c != '\\' || i + 1 >= raw.size()) { cooked.push_back(c); continue; }
            const char e = raw[++i];
            switch (e) {
            case 'n': cooked.push_back('\n'); break;
            case 'r': cooked.push_back('\r'); break;
            case 't': cooked.push_back('\t'); break;
            case 'b': cooked.push_back('\b'); break;
            case 'f': cooked.push_back('\f'); break;
            case 'v': cooked.push_back('\v'); break;
            case '\n': break; // line continuation
            case '\r': if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i; break;
            default: cooked.push_back(e); break;
            }
        }
        return cooked;
    }

    std::unique_ptr<TemplateLiteralNode> _parse_template_literal() {
        Token chunk = _peek();
        const std::size_t start = chunk.start;
        std::vector<std::unique_ptr<TemplateElementNode>> quasis;
        std::vector<std::unique_ptr<ASTNode>> expressions;

        if (chunk.kind == TokenKind::TEMPLATE_NO_SUBSTITUTION) {
            _tokenizer.advance();
            quasis.push_back(std::make_unique<TemplateElementNode>(chunk.start, chunk.end, std::string(_template_chunk_text(chunk)), _cook_template_text(_template_chunk_text(chunk)), true));
            return std::make_unique<TemplateLiteralNode>(start, chunk.end, std::move(quasis), std::move(expressions));
        }
        if (chunk.kind != TokenKind::TEMPLATE_HEAD) _fail("Expected template literal");

        while (true) {
            const bool first = chunk.kind == TokenKind::TEMPLATE_HEAD;
            (void)first;
            quasis.push_back(std::make_unique<TemplateElementNode>(chunk.start, chunk.end, std::string(_template_chunk_text(chunk)), _cook_template_text(_template_chunk_text(chunk)), false));
            _tokenizer.advance(); // first token inside ${...}
            expressions.push_back(_parse_expression());
            if (_peek().kind != TokenKind::RIGHT_BRACE) _fail("Expected '}' in template substitution");
            _tokenizer.advance_template_continuation();
            chunk = _peek();
            if (chunk.kind != TokenKind::TEMPLATE_MIDDLE && chunk.kind != TokenKind::TEMPLATE_TAIL)
                _fail("Unterminated template literal");
            if (chunk.kind == TokenKind::TEMPLATE_TAIL) {
                quasis.push_back(std::make_unique<TemplateElementNode>(chunk.start, chunk.end, std::string(_template_chunk_text(chunk)), _cook_template_text(_template_chunk_text(chunk)), true));
                const std::size_t end = chunk.end;
                _tokenizer.advance();
                return std::make_unique<TemplateLiteralNode>(start, end, std::move(quasis), std::move(expressions));
            }
            // Middle chunk ends with another ${; continue parsing its expression.
        }
    }

    std::unique_ptr<ASTNode> _try_parse_arrow_function(){
        auto checkpoint = _tokenizer.checkpoint();

        std::vector<std::unique_ptr<ASTNode>> params;
        std::vector<std::unique_ptr<ASTNode>> defaults;
        std::optional<std::size_t> rest_parameter;
        std::size_t arrow_start = _peek().start;

        // x => ...
        if (_peek().kind == TokenKind::IDENTIFIER){
            Token identifier = _peek();
            _tokenizer.advance();

            if (_peek().kind != TokenKind::ARROW){
                _tokenizer.restore(checkpoint);
                return nullptr;
            }

            params.push_back(std::make_unique<IdentifierNode>(identifier));
            defaults.push_back(nullptr);
        }

        // (...) => ...
        else if (_peek().kind == TokenKind::LEFT_PAREN) {
            _tokenizer.advance();

            if (_peek().kind != TokenKind::RIGHT_PAREN) {
                while (true) {
                    const bool is_rest = _peek().kind == TokenKind::ELLIPSIS;
                    if (is_rest) {
                        _tokenizer.advance();
                        if (rest_parameter) {
                            _tokenizer.restore(checkpoint);
                            return nullptr;
                        }
                        rest_parameter = params.size();
                    }
                    try {
                        params.push_back(_parse_pattern(true, false));
                    } catch (const ParseException&) {
                        _tokenizer.restore(checkpoint);
                        return nullptr;
                    }
                    defaults.push_back(nullptr);

                    if (is_rest) {
                        if (_peek().kind == TokenKind::EQUAL || _peek().kind == TokenKind::COMMA) {
                            _tokenizer.restore(checkpoint);
                            return nullptr;
                        }
                        break;
                    }
                    if (_peek().kind == TokenKind::EQUAL) {
                        _tokenizer.advance();
                        defaults.back() = _parse_assignment();
                    }
                    if (_peek().kind != TokenKind::COMMA) break;
                    _tokenizer.advance();
                    if (_peek().kind == TokenKind::RIGHT_PAREN) break;
                }
            }

            if (_peek().kind != TokenKind::RIGHT_PAREN) {
                _tokenizer.restore(checkpoint);
                return nullptr;
            }
            _tokenizer.advance();

            if (_peek().kind != TokenKind::ARROW) {
                _tokenizer.restore(checkpoint);
                return nullptr;
            }
        }
        else {
            return nullptr;
        }

        _consume(TokenKind::ARROW);

        FunctionContextGuard guard(_function_depth, _loop_depth, _breakable_depth, _async_function_depth,
            _generator_function_depth, _is_async, _is_generator);
        GrammarContext function_context = _grammar_context;
        function_context.allow_return = true;
        function_context.allow_yield = _is_generator;
        function_context.allow_await = _is_async;
        GrammarContextGuard grammar_guard(_grammar_context, function_context);

        std::unique_ptr<ArrowFunctionExprNode> node;
        if (_peek().kind == TokenKind::LEFT_BRACE) {
            auto body = _parse_block_statement();
            node = std::make_unique<ArrowFunctionExprNode>(arrow_start, std::move(params), std::move(body), false);
        } else {
            auto body = _parse_assignment();
            node = std::make_unique<ArrowFunctionExprNode>(arrow_start, std::move(params), std::move(body), true);
        }
        node->param_defaults = std::move(defaults);
        node->rest_parameter = rest_parameter;
        return node;
    }

    std::unique_ptr<RegExpLiteralNode> _parse_regexp_literal(){
        auto result = _tokenizer.scan_regexp();

        auto node = std::make_unique<RegExpLiteralNode>(result.token.start, result.token.end, result.pattern, result.flags);

        _tokenizer.advance();

        return node;
    }

    std::unique_ptr<ImportDeclarationNode> _parse_import_declaration(){
        Token import_token =
        _consume(TokenKind::IMPORT);

        _consume(TokenKind::LEFT_BRACE);

        std::vector<std::unique_ptr<ImportSpecifierNode>>
            specifiers;

        if (_peek().kind != TokenKind::RIGHT_BRACE) {
            specifiers = _parse_import_specifiers();
        }
        
        _consume(TokenKind::RIGHT_BRACE);
        _consume(TokenKind::FROM);

        Token source_token =
            _consume(TokenKind::STRING);

        auto source =
            std::make_unique<StringLiteralNode>(
                source_token
            );

        std::size_t end =
            _consume_semicolon_or_insert();

        return std::make_unique<ImportDeclarationNode>(
            import_token,
            std::move(specifiers),
            std::move(source),
            end
        );

    }

    std::vector<std::unique_ptr<ImportSpecifierNode>> _parse_import_specifiers(){
        std::vector<std::unique_ptr<ImportSpecifierNode>> specifiers;

        while (true) {
            Token identifier_token =
                _consume(TokenKind::IDENTIFIER);

            auto imported =
                std::make_unique<IdentifierNode>(identifier_token);

            auto local =
                std::make_unique<IdentifierNode>(identifier_token);

            specifiers.push_back(
                std::make_unique<ImportSpecifierNode>(
                    std::move(imported),
                    std::move(local)
                )
            );

            if (_peek().kind != TokenKind::COMMA) {
                break;
            }

            _tokenizer.advance();
        }

        return specifiers;
            
    }

    std::unique_ptr<ExportNamedDeclarationNode> _parse_export_specifiers(Token export_token){
        _consume(TokenKind::LEFT_BRACE);

        std::vector<std::unique_ptr<IdentifierNode>>
            specifiers;

        if (_peek().kind != TokenKind::RIGHT_BRACE){
            while (true){
                Token name = _consume(TokenKind::IDENTIFIER);

                specifiers.push_back(std::make_unique<IdentifierNode>(name));

                if (_peek().kind != TokenKind::COMMA){
                    break;
                }

                _tokenizer.advance();
            }
        }

        _consume(TokenKind::RIGHT_BRACE);

        std::size_t end =
        _consume_semicolon_or_insert();

        return std::make_unique<ExportNamedDeclarationNode>(
            export_token,
            nullptr,
            std::move(specifiers),
            end
        );
    }

    std::unique_ptr<ExportNamedDeclarationNode> _parse_export_declaration() {

        Token export_token =
            _consume(TokenKind::EXPORT);

        if (_peek().kind == TokenKind::LEFT_BRACE){
            return _parse_export_specifiers(export_token);
        }

        if (_peek().kind == TokenKind::FUNCTION) {
            auto declaration =
                _parse_function_declaration();

            std::size_t end = declaration->end;

            return std::make_unique<ExportNamedDeclarationNode>(
                export_token,
                std::move(declaration),
                std::vector<std::unique_ptr<IdentifierNode>>{},
                end
            );
        }

        if (
            _peek().kind == TokenKind::LET ||
            _peek().kind == TokenKind::CONST ||
            _peek().kind == TokenKind::VAR
        ) {
            auto declaration =
                _parse_variable_declaration();

            std::size_t end = declaration->end;

            return std::make_unique<ExportNamedDeclarationNode>(
                export_token,
                std::move(declaration),
                std::vector<std::unique_ptr<IdentifierNode>>{},
                end
            );
        }

        
       _fail(
            "Expected declaration or export list after export"
        );
    }

    std::unique_ptr<MethodDefinitionNode> _parse_method_definition(){
        Token key_token = _consume(TokenKind::IDENTIFIER);

        auto key = std::make_unique<IdentifierNode>(key_token);

        _consume(TokenKind::LEFT_PAREN);

        auto parsed_params =
            _parse_parameters();

        _consume(TokenKind::RIGHT_PAREN);

        FunctionContextGuard guard(_function_depth, _loop_depth, _breakable_depth, _async_function_depth, 
            _generator_function_depth, _is_async, _is_generator);
        GrammarContext function_context = _grammar_context;
        function_context.allow_return = true;
        function_context.allow_yield = _is_generator;
        function_context.allow_await = _is_async;
        GrammarContextGuard grammar_guard(_grammar_context, function_context);

        auto body = _parse_block_statement();

        auto node = std::make_unique<MethodDefinitionNode>(
            std::move(key),
            std::move(parsed_params.params),
            std::move(body)
        );
        node->param_defaults = std::move(parsed_params.defaults);
        node->rest_parameter = parsed_params.rest_parameter;
        return node;
    }

    std::unique_ptr<ClassDeclarationNode> _parse_class_declaration() {

        Token class_token =
            _consume(TokenKind::CLASS);

        Token id_token =
            _consume(TokenKind::IDENTIFIER);

        auto id =
            std::make_unique<IdentifierNode>(id_token);

        std::unique_ptr<ASTNode> super_class;

        if (_peek().kind == TokenKind::EXTENDS) {
            _tokenizer.advance();

            Token super_token =
                _consume(TokenKind::IDENTIFIER);

            super_class =
                std::make_unique<IdentifierNode>(
                    super_token
                );
        }

        _consume(TokenKind::LEFT_BRACE);

        std::vector<
            std::unique_ptr<MethodDefinitionNode>
        > methods;

        while (
            _peek().kind != TokenKind::RIGHT_BRACE &&
            _peek().kind != TokenKind::END_OF_FILE
        ) {
            const std::size_t before = _peek().start;
            methods.push_back(
                _parse_method_definition()
            );
            if (_peek().start == before && _peek().kind != TokenKind::END_OF_FILE) {
                _fail("Internal parser error: class parser made no progress");
            }
        }

        Token right_brace =
            _consume(TokenKind::RIGHT_BRACE);

        return std::make_unique<ClassDeclarationNode>(
            class_token,
            std::move(id),
            std::move(super_class),
            std::move(methods),
            right_brace.end
        );
    }

    std::unique_ptr<ASTNode> _parse_primary(){

        Token token = _peek();

        if (token.kind == TokenKind::NUMBER){
            _tokenizer.advance();
            return std::make_unique<NumberLiteralNode>(token);
        }

        if (token.kind == TokenKind::STRING){
            _tokenizer.advance();
            return std::make_unique<StringLiteralNode>(token);
        }

        if (_is_identifier_reference(token.kind)){
            _tokenizer.advance();
            return std::make_unique<IdentifierNode>(token);
        }

        if (token.kind == TokenKind::TRUE || token.kind == TokenKind::FALSE){
            _tokenizer.advance();
            return std::make_unique<BooleanLiteralNode>(token);
        }

        if (token.kind == TokenKind::NULL_LITERAL){
            _tokenizer.advance();
            return std::make_unique<NullLiteralNode>(token);
        }

        if (token.kind == TokenKind::THIS){
            _tokenizer.advance();
            return std::make_unique<ThisExprNode>(token);
        }

        if (token.kind == TokenKind::FUNCTION){
            return _parse_function_expression();
        }

        if (token.kind == TokenKind::NEW){
            _tokenizer.advance();
            auto callee = _parse_primary();
            std::vector<std::unique_ptr<ASTNode>> args;
            std::size_t end_position = callee->end;
            if (_peek().kind == TokenKind::LEFT_PAREN) {
                _consume(TokenKind::LEFT_PAREN);
                args = _parse_arguments();
                Token right_paren = _consume(TokenKind::RIGHT_PAREN);
                end_position = right_paren.end;
            }
            return std::make_unique<NewExprNode>(std::move(callee), std::move(args), end_position);
        }

        if (token.kind == TokenKind::LEFT_PAREN){
            _tokenizer.advance(); // consume '('
            auto expr = _parse_expression();
            _consume(TokenKind::RIGHT_PAREN); // consume ')'
            expr->parenthesized = true;
            return expr;
        }

        if (token.kind == TokenKind::LEFT_BRACKET) return _parse_array_expression();

        if (token.kind == TokenKind::LEFT_BRACE) return _parse_object_expression();

        if (token.kind == TokenKind::SLASH) return _parse_regexp_literal();

        if (token.kind == TokenKind::TEMPLATE_HEAD || token.kind == TokenKind::TEMPLATE_NO_SUBSTITUTION)
            return _parse_template_literal();

        _fail("Expected primary expression");

    }

    std::unique_ptr<ASTNode> _parse_member_call(){
        auto object = _parse_primary();
        bool in_optional_chain = false;
        std::unique_ptr<ASTNode> chain_base;
        std::vector<OptionalChainSegment> segments;
        std::size_t chain_end = object->end;

        auto begin_chain = [&]() {
            if (!in_optional_chain) {
                chain_base = std::move(object);
                in_optional_chain = true;
            }
        };

        while (true) {
            const auto kind = _peek().kind;

            if (kind == TokenKind::OPTIONAL_CHAIN) {
                if (!in_optional_chain && object->type == ASTNodeType::NEW_EXPR && !object->parenthesized)
                    _fail("Optional chain cannot directly follow a new expression");
                begin_chain();
                _tokenizer.advance();
                OptionalChainSegment segment;
                segment.optional = true;

                if (_peek().kind == TokenKind::LEFT_PAREN) {
                    _consume(TokenKind::LEFT_PAREN);
                    segment.kind = OptionalChainSegmentKind::CALL;
                    segment.arguments = _parse_arguments();
                    const Token right = _consume(TokenKind::RIGHT_PAREN);
                    segment.end = right.end;
                } else if (_peek().kind == TokenKind::LEFT_BRACKET) {
                    _consume(TokenKind::LEFT_BRACKET);
                    segment.kind = OptionalChainSegmentKind::COMPUTED_PROPERTY;
                    segment.property = _parse_expression();
                    const Token right = _consume(TokenKind::RIGHT_BRACKET);
                    segment.end = right.end;
                } else {
                    if (!_is_identifier_name_token(_peek().kind))
                        _fail("Expected property name, '[' or '(' after '?.'");
                    const Token property_token = _peek();
                    _tokenizer.advance();
                    segment.kind = OptionalChainSegmentKind::STATIC_PROPERTY;
                    segment.property = std::make_unique<IdentifierNode>(property_token);
                    segment.end = property_token.end;
                }
                chain_end = segment.end;
                segments.push_back(std::move(segment));
                continue;
            }

            if (kind == TokenKind::TEMPLATE_HEAD || kind == TokenKind::TEMPLATE_NO_SUBSTITUTION) {
                if (in_optional_chain) _fail("Tagged template cannot follow an optional chain");
                auto quasi = _parse_template_literal();
                object = std::make_unique<TaggedTemplateExprNode>(std::move(object), std::move(quasi));
                continue;
            }

            if (kind != TokenKind::DOT && kind != TokenKind::LEFT_BRACKET && kind != TokenKind::LEFT_PAREN)
                break;

            if (in_optional_chain) {
                OptionalChainSegment segment;
                segment.optional = false;
                if (kind == TokenKind::DOT) {
                    _tokenizer.advance();
                    if (!_is_identifier_name_token(_peek().kind)) _fail("Expected IdentifierName after '.'");
                    const Token property_token = _peek();
                    _tokenizer.advance();
                    segment.kind = OptionalChainSegmentKind::STATIC_PROPERTY;
                    segment.property = std::make_unique<IdentifierNode>(property_token);
                    segment.end = property_token.end;
                } else if (kind == TokenKind::LEFT_BRACKET) {
                    _tokenizer.advance();
                    segment.kind = OptionalChainSegmentKind::COMPUTED_PROPERTY;
                    segment.property = _parse_expression();
                    const Token right = _consume(TokenKind::RIGHT_BRACKET);
                    segment.end = right.end;
                } else {
                    _consume(TokenKind::LEFT_PAREN);
                    segment.kind = OptionalChainSegmentKind::CALL;
                    segment.arguments = _parse_arguments();
                    const Token right = _consume(TokenKind::RIGHT_PAREN);
                    segment.end = right.end;
                }
                chain_end = segment.end;
                segments.push_back(std::move(segment));
                continue;
            }

            if (kind == TokenKind::DOT) {
                _tokenizer.advance();
                if (!_is_identifier_name_token(_peek().kind)) _fail("Expected IdentifierName after '.'");
                const Token property_token = _peek();
                _tokenizer.advance();
                auto property = std::make_unique<IdentifierNode>(property_token);
                const auto end_position = property->end;
                object = std::make_unique<MemberExprNode>(std::move(object), std::move(property), false, end_position);
                continue;
            }
            if (kind == TokenKind::LEFT_BRACKET) {
                _tokenizer.advance();
                auto property = _parse_expression();
                const Token right = _consume(TokenKind::RIGHT_BRACKET);
                object = std::make_unique<MemberExprNode>(std::move(object), std::move(property), true, right.end);
                continue;
            }
            _consume(TokenKind::LEFT_PAREN);
            auto args = _parse_arguments();
            const Token right = _consume(TokenKind::RIGHT_PAREN);
            object = std::make_unique<CallExprNode>(std::move(object), std::move(args), right.end);
        }

        if (in_optional_chain)
            return std::make_unique<OptionalChainExprNode>(std::move(chain_base), std::move(segments), chain_end);
        return object;
    }

    std::unique_ptr<ASTNode> _parse_postfix(){
        auto arg = _parse_member_call();

        if (_peek().kind == TokenKind::PLUS_PLUS || _peek().kind == TokenKind::MINUS_MINUS){
            Token op_token = _peek();

            if (!_is_simple_assignment_target(arg.get())){
                _fail("Invalid update target");
                
            }

            _tokenizer.advance();
            arg = std::make_unique<UpdateExprNode>(op_token, std::move(arg), false);

            if (
                _peek().kind == TokenKind::PLUS_PLUS ||
                _peek().kind == TokenKind::MINUS_MINUS
            ) {
                _fail(
                    "Invalid consecutive postfix update"
                );
            }
        }

        return arg;
    }

    std::unique_ptr<ASTNode> _parse_unary(){
        Token op_token = _peek();

        // prefix update
        if ( op_token.kind == TokenKind::PLUS_PLUS || op_token.kind == TokenKind::MINUS_MINUS){
            _tokenizer.advance();

            auto arg = _parse_unary();

            if (!_is_simple_assignment_target(arg.get())) {
                _fail("Invalid update target");
            }

            return std::make_unique<UpdateExprNode>(
                op_token,
                std::move(arg),
                true
            );
        }

        if (
                op_token.kind == TokenKind::BANG ||
                op_token.kind == TokenKind::TILDE ||
                op_token.kind == TokenKind::MINUS ||
                op_token.kind == TokenKind::PLUS ||
                op_token.kind == TokenKind::DELETE ||
                op_token.kind == TokenKind::VOID ||
                op_token.kind == TokenKind::TYPEOF
            ) {
                _tokenizer.advance();

                auto arg = _parse_unary();

                return std::make_unique<UnaryExprNode>(
                    op_token,
                    std::move(arg)
                );
            }


        return _parse_postfix();
        
    }

    std::unique_ptr<ASTNode> _parse_exponentiation() {
        // ECMAScript does not permit an unparenthesized UnaryExpression on
        // the left of '**'. Parenthesized unary expressions arrive here via
        // PrimaryExpression and are therefore valid exponentiation bases.
        switch (_peek().kind) {
            case TokenKind::BANG:
            case TokenKind::TILDE:
            case TokenKind::MINUS:
            case TokenKind::PLUS:
            case TokenKind::DELETE:
            case TokenKind::VOID:
            case TokenKind::TYPEOF:
                return _parse_unary();
            default:
                break;
        }

        auto left = _parse_unary();
        if (_peek().kind != TokenKind::STAR_STAR) return left;

        Token op_token = _peek();
        _tokenizer.advance();
        auto right = _parse_exponentiation();
        return std::make_unique<BinaryExprNode>(
            op_token.kind, std::move(left), std::move(right));
    }

    std::unique_ptr<ASTNode> _parse_multiplicative() {
        return _parse_left_associative(
            &Parser::_parse_exponentiation,
            {TokenKind::STAR, TokenKind::SLASH, TokenKind::PERCENT});
    }

    std::unique_ptr<ASTNode> _parse_additive() {
        return _parse_left_associative(
            &Parser::_parse_multiplicative,
            {TokenKind::PLUS, TokenKind::MINUS});
    }

    std::unique_ptr<ASTNode> _parse_shift() {
        return _parse_left_associative(
            &Parser::_parse_additive,
            {TokenKind::SHIFT_LEFT, TokenKind::SHIFT_RIGHT, TokenKind::SHIFT_RIGHT_UNSIGNED});
    }

    std::unique_ptr<ASTNode> _parse_relational() {
        auto left = _parse_shift();
        while (true) {
            const TokenKind kind = _peek().kind;
            const bool ordinary = _matches(kind, {
                TokenKind::LESS, TokenKind::LESS_EQUAL,
                TokenKind::GREATER, TokenKind::GREATER_EQUAL,
                TokenKind::INSTANCEOF
            });
            const bool in_operator = kind == TokenKind::IN && _grammar_context.allow_in;
            if (!ordinary && !in_operator) break;

            Token op_token = _peek();
            _tokenizer.advance();
            auto right = _parse_shift();
            left = std::make_unique<BinaryExprNode>(
                op_token.kind, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<ASTNode> _parse_equality() {
        return _parse_left_associative(
            &Parser::_parse_relational,
            {TokenKind::EQUAL_EQUAL, TokenKind::BANG_EQUAL,
             TokenKind::EQUAL_EQUAL_EQUAL, TokenKind::BANG_EQUAL_EQUAL});
    }

    std::unique_ptr<ASTNode> _parse_bitwise_and() {
        return _parse_left_associative(&Parser::_parse_equality, {TokenKind::AMPERSAND});
    }

    std::unique_ptr<ASTNode> _parse_bitwise_xor() {
        return _parse_left_associative(&Parser::_parse_bitwise_and, {TokenKind::CARET});
    }

    std::unique_ptr<ASTNode> _parse_bitwise_or() {
        return _parse_left_associative(&Parser::_parse_bitwise_xor, {TokenKind::PIPE});
    }

    std::unique_ptr<ASTNode> _parse_logical_and() {
        return _parse_left_associative(&Parser::_parse_bitwise_or, {TokenKind::AND_AND}, true);
    }

    std::unique_ptr<ASTNode> _parse_logical_or() {
        return _parse_left_associative(&Parser::_parse_logical_and, {TokenKind::OR_OR}, true);
    }

    std::unique_ptr<ASTNode> _parse_coalesce() {
        auto left = _parse_logical_or();
        while (_peek().kind == TokenKind::NULLISH) {
            auto mixed_unparenthesized = [](const ASTNode* node) {
                if (node == nullptr || node->parenthesized || node->type != ASTNodeType::LOGICAL_EXPR) return false;
                const auto& logical = static_cast<const LogicalExprNode&>(*node);
                return logical.op == TokenKind::AND_AND || logical.op == TokenKind::OR_OR;
            };
            if (mixed_unparenthesized(left.get()))
                _fail("Cannot mix nullish coalescing with logical AND/OR without parentheses");
            _tokenizer.advance();
            auto right = _parse_logical_or();
            if (mixed_unparenthesized(right.get()))
                _fail("Cannot mix nullish coalescing with logical AND/OR without parentheses");
            left = std::make_unique<LogicalExprNode>(TokenKind::NULLISH, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<ASTNode> _parse_conditional() {
        auto test = _parse_coalesce();
        if (_peek().kind != TokenKind::QUESTION) return test;

        _tokenizer.advance();
        // ConditionalExpression[?In] always parses its first branch as
        // AssignmentExpression[+In]. The alternate inherits the surrounding
        // In grammar parameter. This matters in NoIn contexts such as the
        // initializer of a classic for statement.
        std::unique_ptr<ASTNode> consequent;
        {
            GrammarContext consequent_context = _grammar_context;
            consequent_context.allow_in = true;
            GrammarContextGuard guard(_grammar_context, consequent_context);
            consequent = _parse_assignment();
        }
        _consume(TokenKind::COLON);
        auto alternate = _parse_assignment();
        return std::make_unique<ConditionalExprNode>(
            std::move(test), std::move(consequent), std::move(alternate));
    }

    bool _is_contextual_identifier_reference(TokenKind kind) const noexcept {
        if (kind == TokenKind::AWAIT) return !_grammar_context.allow_await;
        if (kind == TokenKind::YIELD) return !_grammar_context.allow_yield;
        return false;
    }

    bool _is_identifier_reference(TokenKind kind) const noexcept {
        return kind == TokenKind::IDENTIFIER || _is_contextual_identifier_reference(kind);
    }

    std::unique_ptr<IdentifierNode> _consume_identifier_reference() {
        if (!_is_identifier_reference(_peek().kind)) _fail("Expected IdentifierReference");
        Token token = _peek();
        _tokenizer.advance();
        return std::make_unique<IdentifierNode>(token);
    }

    bool _is_assignment_operator(TokenKind kind) const {
        switch (kind) {
            case TokenKind::EQUAL:
            case TokenKind::PLUS_EQUAL:
            case TokenKind::MINUS_EQUAL:
            case TokenKind::STAR_EQUAL:
            case TokenKind::STAR_STAR_EQUAL:
            case TokenKind::SLASH_EQUAL:
            case TokenKind::PERCENT_EQUAL:
            case TokenKind::SHIFT_LEFT_EQUAL:
            case TokenKind::SHIFT_RIGHT_EQUAL:
            case TokenKind::SHIFT_RIGHT_UNSIGNED_EQUAL:
            case TokenKind::AMPERSAND_EQUAL:
            case TokenKind::CARET_EQUAL:
            case TokenKind::PIPE_EQUAL:
            case TokenKind::AND_AND_EQUAL:
            case TokenKind::OR_OR_EQUAL:
            case TokenKind::NULLISH_EQUAL:
                return true;
            default:
                return false;
        }
    }

    std::unique_ptr<ASTNode> _parse_assignment(){
        if (_peek().kind == TokenKind::YIELD && _grammar_context.allow_yield) {
            Token yield_token = _consume(TokenKind::YIELD);
            std::unique_ptr<ASTNode> argument;
            if (_peek().kind != TokenKind::SEMICOLON &&
                _peek().kind != TokenKind::RIGHT_BRACE &&
                !_peek().line_break_before) {
                argument = _parse_assignment();
            }
            return std::make_unique<YieldExprNode>(yield_token, std::move(argument));
        }

        if (auto arrow = _try_parse_arrow_function()){
            return arrow;
        }

        if (_peek().kind == TokenKind::LEFT_BRACKET || _peek().kind == TokenKind::LEFT_BRACE) {
            auto checkpoint = _tokenizer.checkpoint();
            try {
                auto pattern = _parse_pattern(false, false);
                if (_peek().kind == TokenKind::EQUAL) {
                    _tokenizer.advance();
                    auto right = _parse_assignment();
                    return std::make_unique<AssignmentExprNode>(TokenKind::EQUAL, std::move(pattern), std::move(right));
                }
            } catch (const ParseException&) {
            }
            _tokenizer.restore(checkpoint);
        }

        auto left = _parse_conditional();
        Token op_token = _peek();

        if (_is_assignment_operator(op_token.kind)) {
            const auto target_kind = _classify_assignment_target(left.get());
            if (target_kind == AssignmentTargetKind::INVALID ||
                (target_kind == AssignmentTargetKind::PATTERN && op_token.kind != TokenKind::EQUAL)) {
                _fail("Invalid assignment target");
            }

            _tokenizer.advance();
            auto right = _parse_assignment();
            return std::make_unique<AssignmentExprNode>(
                op_token.kind, std::move(left), std::move(right));
        }

        return left;
    }

    std::unique_ptr<ASTNode> _parse_expression() {
        std::vector<std::unique_ptr<ASTNode>> expressions;
        expressions.push_back(_parse_assignment());

        while (_peek().kind == TokenKind::COMMA) {
            _tokenizer.advance();
            expressions.push_back(_parse_assignment());
        }

        if (expressions.size() == 1) return std::move(expressions.front());
        return std::make_unique<SequenceExprNode>(std::move(expressions));
    }

    std::unique_ptr<ASTNode> _parse_expression_statement(){
        auto expression = _parse_expression();

        std::size_t end = _consume_semicolon_or_insert();

        return std::make_unique<ExpressionStatementNode>(std::move(expression), end);
    }

    std::unique_ptr<ASTNode> _parse_empty_statement(){
        Token semicolon = _consume(TokenKind::SEMICOLON);

        return std::make_unique<EmptyStatementNode>(semicolon);
    }

    std::unique_ptr<BlockStatementNode> _parse_block_statement(){
        Token left_brace = _consume(TokenKind::LEFT_BRACE);

        std::vector<std::unique_ptr<ASTNode>> body;

        while (
            _peek().kind != TokenKind::RIGHT_BRACE &&
            _peek().kind != TokenKind::END_OF_FILE
        ) {
            const std::size_t before = _peek().start;
            body.push_back(_parse_statement());
            if (_peek().start == before && _peek().kind != TokenKind::END_OF_FILE) {
                _fail("Internal parser error: statement parser made no progress");
            }
        }

        Token right_brace = _consume(TokenKind::RIGHT_BRACE);

        return std::make_unique<BlockStatementNode>(left_brace, right_brace, std::move(body));
    }

    std::unique_ptr<ASTNode> _parse_return_statement(){
        Token return_token = _consume(TokenKind::RETURN);

        if (!_grammar_context.allow_return && _function_depth == 0) {
            _fail(
                "Return statement outside function"
            );
        }

        std::unique_ptr<ASTNode> argument;

        if (_peek().kind != TokenKind::SEMICOLON &&
            _peek().kind != TokenKind::RIGHT_BRACE &&
            _peek().kind != TokenKind::END_OF_FILE &&
            !_peek().line_break_before){

            argument = _parse_expression();
        }else{
            argument = nullptr;
        }

        // Token semicolon = _consume(TokenKind::SEMICOLON);
        std::size_t end = _consume_semicolon_or_insert();

        return std::make_unique<ReturnStatementNode>(return_token, std::move(argument), end);

    }

    std::unique_ptr<ASTNode> _parse_throw_statement(){
        Token throw_token = _consume(TokenKind::THROW);
        if (_peek().line_break_before) {
            _fail("Line terminator is not allowed after throw");
        }
        if (_peek().kind == TokenKind::SEMICOLON ||
            _peek().kind == TokenKind::RIGHT_BRACE ||
            _peek().kind == TokenKind::END_OF_FILE) {
            _fail("Throw statement requires an expression");
        }
        auto argument = _parse_assignment();
        std::size_t end = _consume_semicolon_or_insert();
        return std::make_unique<ThrowStatementNode>(throw_token, std::move(argument), end);
    }

    std::unique_ptr<ASTNode> _parse_try_statement(){
        Token try_token = _consume(TokenKind::TRY);
        auto try_block = _parse_block_statement();

        std::unique_ptr<ASTNode> catch_param;
        std::unique_ptr<BlockStatementNode> catch_block;
        std::unique_ptr<BlockStatementNode> finally_block;

        if (_peek().kind == TokenKind::CATCH || _peek().kind == TokenKind::FOR) {
            _tokenizer.advance();
            _consume(TokenKind::LEFT_PAREN);
            catch_param = _parse_pattern(true, false);
            _consume(TokenKind::RIGHT_PAREN);
            catch_block = _parse_block_statement();
        }

        if (_peek().kind == TokenKind::FINALLY) {
            _tokenizer.advance();
            finally_block = _parse_block_statement();
        }

        if (!catch_block && !finally_block) {
            _fail("Try statement requires catch or finally");
        }

        return std::make_unique<TryStatementNode>(
            try_token, std::move(try_block), std::move(catch_param),
            std::move(catch_block), std::move(finally_block));
    }

    std::unique_ptr<IfStatementNode> _parse_if_statement(){
        Token if_token = _consume(TokenKind::IF);

        _consume(TokenKind::LEFT_PAREN);

        auto test = _parse_assignment();

        _consume(TokenKind::RIGHT_PAREN);

        auto consequent = _parse_statement();

        std::unique_ptr<ASTNode> alternate;

        if (_peek().kind == TokenKind::ELSE){
            _tokenizer.advance();
            alternate = _parse_statement();
        }

        return std::make_unique<IfStatementNode>(
            if_token,
            std::move(test),
            std::move(consequent),
            std::move(alternate)
        );
    }

    std::unique_ptr<WhileStatementNode> _parse_while_statement(){
        Token while_token = _consume(TokenKind::WHILE);
        _consume(TokenKind::LEFT_PAREN);
        auto test = _parse_expression();
        _consume(TokenKind::RIGHT_PAREN);
        LoopContextGuard loop_guard(_loop_depth);
        BreakableContextGuard break_guard(_breakable_depth);
        auto body = _parse_statement();
        return std::make_unique<WhileStatementNode>(while_token, std::move(test), std::move(body));
    }

    std::unique_ptr<DoWhileStatementNode> _parse_do_while_statement() {
        Token do_token = _consume(TokenKind::DO);
        std::unique_ptr<ASTNode> body;
        {
            LoopContextGuard loop_guard(_loop_depth);
            BreakableContextGuard break_guard(_breakable_depth);
            body = _parse_statement();
        }
        _consume(TokenKind::WHILE);
        _consume(TokenKind::LEFT_PAREN);
        auto test = _parse_expression();
        _consume(TokenKind::RIGHT_PAREN);
        const std::size_t end = _consume_semicolon_or_insert();
        return std::make_unique<DoWhileStatementNode>(do_token, std::move(body), std::move(test), end);
    }

    std::unique_ptr<VariableDeclarationNode> _parse_for_declaration() {
        Token keyword = _peek();
        VariableKind kind;
        switch (keyword.kind) {
        case TokenKind::LET: kind = VariableKind::LET; break;
        case TokenKind::CONST: kind = VariableKind::CONST; break;
        case TokenKind::VAR: kind = VariableKind::VAR; break;
        default: _fail("Expected variable declaration in for statement");
        }
        _tokenizer.advance();
        std::vector<std::unique_ptr<VariableDeclaratorNode>> declarations;
        while (true) {
            auto id = _parse_pattern(true, false);
            std::unique_ptr<ASTNode> init;
            if (_peek().kind == TokenKind::EQUAL) {
                _tokenizer.advance();
                GrammarContext no_in = _grammar_context;
                no_in.allow_in = false;
                GrammarContextGuard guard(_grammar_context, no_in);
                init = _parse_assignment();
            }
            declarations.push_back(std::make_unique<VariableDeclaratorNode>(std::move(id), std::move(init)));
            if (_peek().kind != TokenKind::COMMA) break;
            _tokenizer.advance();
        }
        const auto& previous = _tokenizer.previous();
        return std::make_unique<VariableDeclarationNode>(keyword, kind, std::move(declarations),
            previous ? previous->end : keyword.end);
    }

    bool _is_of_token() const {
        return _peek().kind == TokenKind::IDENTIFIER && _peek().lexeme == "of";
    }

    std::unique_ptr<ASTNode> _parse_for_statement() {
        Token for_token = _consume(TokenKind::FOR);
        _consume(TokenKind::LEFT_PAREN);

        std::unique_ptr<ASTNode> init;
        if (_peek().kind != TokenKind::SEMICOLON) {
            if (_matches(_peek().kind, {TokenKind::LET, TokenKind::CONST, TokenKind::VAR})) {
                init = _parse_for_declaration();
            } else {
                GrammarContext no_in = _grammar_context;
                no_in.allow_in = false;
                GrammarContextGuard guard(_grammar_context, no_in);
                init = _parse_expression();
            }
        }

        if (_peek().kind == TokenKind::IN || _is_of_token()) {
            const bool is_in = _peek().kind == TokenKind::IN;
            if (!init) _fail("for...in/of requires a left-hand side");
            if (init->type == ASTNodeType::VARIABLE_DECLARATION) {
                const auto& declaration = static_cast<const VariableDeclarationNode&>(*init);
                if (declaration.declarations.size() != 1 || declaration.declarations.front()->init)
                    _fail("for...in/of declaration requires one uninitialized binding");
            } else if (!_is_simple_assignment_target(init.get())) {
                _fail("Invalid for...in/of assignment target");
            }
            _tokenizer.advance();
            auto rhs = _parse_assignment();
            _consume(TokenKind::RIGHT_PAREN);
            std::unique_ptr<ASTNode> body;
            {
                LoopContextGuard loop_guard(_loop_depth);
                BreakableContextGuard break_guard(_breakable_depth);
                body = _parse_statement();
            }
            if (is_in)
                return std::make_unique<ForInStatementNode>(for_token, std::move(init), std::move(rhs), std::move(body));
            return std::make_unique<ForOfStatementNode>(for_token, std::move(init), std::move(rhs), std::move(body));
        }

        if (init && init->type == ASTNodeType::VARIABLE_DECLARATION) {
            const auto& declaration = static_cast<const VariableDeclarationNode&>(*init);
            for (const auto& declarator : declaration.declarations)
                _validate_variable_declarator(declaration.kind, declarator.get());
        }
        _consume(TokenKind::SEMICOLON);
        std::unique_ptr<ASTNode> test;
        if (_peek().kind != TokenKind::SEMICOLON) test = _parse_expression();
        _consume(TokenKind::SEMICOLON);
        std::unique_ptr<ASTNode> update;
        if (_peek().kind != TokenKind::RIGHT_PAREN) update = _parse_expression();
        _consume(TokenKind::RIGHT_PAREN);
        std::unique_ptr<ASTNode> body;
        {
            LoopContextGuard loop_guard(_loop_depth);
            BreakableContextGuard break_guard(_breakable_depth);
            body = _parse_statement();
        }
        return std::make_unique<ForStatementNode>(for_token, std::move(init), std::move(test), std::move(update), std::move(body));
    }

    std::unique_ptr<SwitchStatementNode> _parse_switch_statement() {
        Token switch_token = _consume(TokenKind::SWITCH);
        _consume(TokenKind::LEFT_PAREN);
        auto discriminant = _parse_expression();
        _consume(TokenKind::RIGHT_PAREN);
        _consume(TokenKind::LEFT_BRACE);
        std::vector<SwitchCaseNode> cases;
        bool saw_default = false;
        BreakableContextGuard break_guard(_breakable_depth);
        while (_peek().kind != TokenKind::RIGHT_BRACE) {
            SwitchCaseNode clause;
            Token clause_token = _peek();
            if (_peek().kind == TokenKind::CASE) {
                _tokenizer.advance();
                clause.test = _parse_expression();
            } else if (_peek().kind == TokenKind::DEFAULT) {
                if (saw_default) _fail("Duplicate default clause in switch");
                saw_default = true;
                _tokenizer.advance();
            } else {
                _fail("Expected case or default in switch");
            }
            clause.start = clause_token.start;
            _consume(TokenKind::COLON);
            while (_peek().kind != TokenKind::CASE && _peek().kind != TokenKind::DEFAULT &&
                   _peek().kind != TokenKind::RIGHT_BRACE && _peek().kind != TokenKind::END_OF_FILE) {
                clause.consequent.push_back(_parse_statement());
            }
            const auto& previous = _tokenizer.previous();
            clause.end = previous ? previous->end : clause_token.end;
            cases.push_back(std::move(clause));
        }
        Token right = _consume(TokenKind::RIGHT_BRACE);
        return std::make_unique<SwitchStatementNode>(switch_token, std::move(discriminant), std::move(cases), right.end);
    }

    bool _label_resolves(std::string_view name, bool require_iteration) const {
        for (auto it = _active_labels.rbegin(); it != _active_labels.rend(); ++it) {
            if (it->function_depth != _function_depth) continue;
            if (it->name == name) return !require_iteration || it->iteration;
        }
        return false;
    }

    bool _labeled_body_is_iteration() {
        auto checkpoint = _tokenizer.checkpoint();
        while (_peek().kind == TokenKind::IDENTIFIER) {
            _tokenizer.advance();
            if (_peek().kind != TokenKind::COLON) { _tokenizer.restore(checkpoint); return false; }
            _tokenizer.advance();
        }
        const bool result = _matches(_peek().kind, {TokenKind::WHILE, TokenKind::DO, TokenKind::FOR});
        _tokenizer.restore(checkpoint);
        return result;
    }

    std::unique_ptr<LabeledStatementNode> _parse_labeled_statement() {
        Token label = _consume(TokenKind::IDENTIFIER);
        _consume(TokenKind::COLON);
        for (const auto& active : _active_labels) {
            if (active.function_depth == _function_depth && active.name == label.lexeme)
                _fail("Duplicate label");
        }
        _active_labels.push_back(ActiveLabel{std::string(label.lexeme), _labeled_body_is_iteration(), _function_depth});
        auto body = _parse_statement();
        _active_labels.pop_back();
        return std::make_unique<LabeledStatementNode>(label, std::move(body));
    }

    std::unique_ptr<BreakStatementNode> _parse_break_statement() {
        Token break_token = _consume(TokenKind::BREAK);
        std::optional<std::string> label;
        if (!_peek().line_break_before && _peek().kind == TokenKind::IDENTIFIER) {
            label = std::string(_peek().lexeme);
            _tokenizer.advance();
            if (!_label_resolves(*label, false)) _fail("Unknown break label");
        } else if (_breakable_depth == 0) {
            _fail("Break statement outside breakable statement");
        }
        std::size_t end = _consume_semicolon_or_insert();
        return std::make_unique<BreakStatementNode>(break_token, std::move(label), end);
    }

    std::unique_ptr<ContinueStatementNode> _parse_continue_statement() {
        Token continue_token = _consume(TokenKind::CONTINUE);
        std::optional<std::string> label;
        if (!_peek().line_break_before && _peek().kind == TokenKind::IDENTIFIER) {
            label = std::string(_peek().lexeme);
            _tokenizer.advance();
            if (!_label_resolves(*label, true)) _fail("Continue label is not an active iteration label");
        } else if (_loop_depth == 0) {
            _fail("Continue statement outside loop");
        }
        std::size_t end = _consume_semicolon_or_insert();
        return std::make_unique<ContinueStatementNode>(continue_token, std::move(label), end);
    }

    std::unique_ptr<DebuggerStatementNode> _parse_debugger_statement() {
        Token token = _consume(TokenKind::DEBUGGER);
        return std::make_unique<DebuggerStatementNode>(token, _consume_semicolon_or_insert());
    }

    std::unique_ptr<ASTNode> _parse_statement() {
        if (_peek().kind == TokenKind::IDENTIFIER) {
            auto checkpoint = _tokenizer.checkpoint();
            _tokenizer.advance();
            const bool labeled = _peek().kind == TokenKind::COLON;
            _tokenizer.restore(checkpoint);
            if (labeled) return _parse_labeled_statement();
        }
        switch (_peek().kind) {
            case TokenKind::SEMICOLON:
                return _parse_empty_statement();
            case TokenKind::LEFT_BRACE:
                return _parse_block_statement();
            case TokenKind::RETURN:
                return _parse_return_statement();
            case TokenKind::THROW:
                return _parse_throw_statement();
            case TokenKind::TRY:
                return _parse_try_statement();
            case TokenKind::LET:
            case TokenKind::CONST:
            case TokenKind::VAR:
                return _parse_variable_declaration();
            case TokenKind::FUNCTION:
                return _parse_function_declaration();
            case TokenKind::IF:
                return _parse_if_statement();
            case TokenKind::WHILE:
                return _parse_while_statement();
            case TokenKind::DO:
                return _parse_do_while_statement();
            case TokenKind::FOR:
                return _parse_for_statement();
            case TokenKind::SWITCH:
                return _parse_switch_statement();
            case TokenKind::BREAK:
                return _parse_break_statement();
            case TokenKind::CONTINUE:
                return _parse_continue_statement();
            case TokenKind::DEBUGGER:
                return _parse_debugger_statement();
            case TokenKind::CLASS:
                return _parse_class_declaration();

            default:
                return _parse_expression_statement();
        }
    }

    std::unique_ptr<ASTNode> _parse_top_level_statement() {
        switch (_peek().kind) {
            case TokenKind::IMPORT:
                return _parse_import_declaration();

            case TokenKind::EXPORT:
                return _parse_export_declaration();

            default:
                return _parse_statement();
        }
    }

public:
    Parser(std::string_view input):_tokenizer(input){}

    
    std::unique_ptr<ASTNode> parse_statement() {
        if (_peek().kind == TokenKind::ERROR) {
            _fail("Tokenizer error encountered");
        }
        return _parse_top_level_statement();
    }

    // Full program parser entry point
    std::unique_ptr<ASTNode> parse_program() {
        auto program = std::make_unique<ProgramNode>(_tokenizer.input_length());

        while (!_tokenizer.is_at_end()) {
            if (_peek().kind == TokenKind::ERROR) {
                _fail("Tokenizer error encountered");
            }

            const std::size_t before = _peek().start;
            program->body.push_back(_parse_top_level_statement());
            if (_peek().start == before && _peek().kind != TokenKind::END_OF_FILE) {
                _fail("Internal parser error: top-level parser made no progress");
            }
        }

        return program;
    }

};


} // namespace js::frontend

#endif
