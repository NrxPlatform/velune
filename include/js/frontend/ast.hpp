#ifndef AST_HPP
#define AST_HPP

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <js/frontend/token.hpp>

namespace js::frontend {


enum class ASTNodeType {
    PROGRAM,

    // Leafes
    NUMBER_LITERAL,
    STRING_LITERAL,
    BOOLEAN_LITERAL,
    NULL_LITERAL,
    IDENTIFIER,
    THIS_EXPR,

    // Expressions
    UNARY_EXPR,
    UPDATE_EXPR,
    BINARY_EXPR,
    LOGICAL_EXPR,
    CONDITIONAL_EXPR,
    SEQUENCE_EXPR,
    CALL_EXPR,
    NEW_EXPR,
    MEMBER_EXPR,
    ASSIGNMENT_EXPR,
    SPREAD_ELEMENT,
    TAGGED_TEMPLATE_EXPR,
    OPTIONAL_CHAIN_EXPR,

    // Patterns
    ARRAY_PATTERN,
    OBJECT_PATTERN,
    OBJECT_PATTERN_PROPERTY,
    ASSIGNMENT_PATTERN,
    REST_ELEMENT,

    // NEW expressions
    ARRAY_EXPR,
    OBJECT_EXPR,
    PROPERTY,
    FUNCTION_EXPR,
    ARROW_FUNCTION_EXPR,
    REGEXP_LITERAL,
    AWAIT_EXPR,
    YIELD_EXPR,
    TEMPLATE_LITERAL,
    TEMPLATE_ELEMENT,

    // Statements
    EXPRESSION_STATEMENT,
    EMPTY_STATEMENT,
    BLOCK_STATEMENT,
    RETURN_STATEMENT,
    THROW_STATEMENT,
    TRY_STATEMENT,

    // NEW control-flow
    IF_STATEMENT,
    WHILE_STATEMENT,
    DO_WHILE_STATEMENT,
    FOR_STATEMENT,
    FOR_IN_STATEMENT,
    FOR_OF_STATEMENT,
    SWITCH_STATEMENT,
    LABELED_STATEMENT,
    BREAK_STATEMENT,
    CONTINUE_STATEMENT,
    DEBUGGER_STATEMENT,

    // Declarations
    VARIABLE_DECLARATION,
    VARIABLE_DECLARATOR,
    FUNCTION_DECLARATION,

    // NEW modules
    IMPORT_DECLARATION,
    IMPORT_SPECIFIER,
    EXPORT_NAMED_DECLARATION,

    // NEW classes
    CLASS_DECLARATION,
    METHOD_DEFINITION
};


enum class VariableKind {
    LET,
    CONST,
    VAR
};


struct ASTNode {
    ASTNodeType type;
    std::size_t start;
    std::size_t end;

    ASTNode(
        ASTNodeType node_type,
        std::size_t node_start,
        std::size_t node_end
    )
        : type(node_type),
          start(node_start),
          end(node_end)
    {}

    virtual ~ASTNode() = default;

    bool parenthesized{false};
};


// ============================================================
// Program
// ============================================================

struct ProgramNode : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> body;

    explicit ProgramNode(std::size_t source_length)
        : ASTNode(
            ASTNodeType::PROGRAM,
            0,
            source_length
        )
    {}
};


// ============================================================
// Leaf expressions
// ============================================================

struct NumberLiteralNode : ASTNode {
    std::string_view value;

    explicit NumberLiteralNode(const Token& token)
        : ASTNode(
            ASTNodeType::NUMBER_LITERAL,
            token.start,
            token.end
        ),
          value(token.lexeme)
    {}
};


struct StringLiteralNode : ASTNode {
    std::string_view value;

    explicit StringLiteralNode(const Token& token)
        : ASTNode(
            ASTNodeType::STRING_LITERAL,
            token.start,
            token.end
        ),
          value(token.lexeme)
    {}
};


struct BooleanLiteralNode : ASTNode {
    bool value;

    explicit BooleanLiteralNode(const Token& token)
        : ASTNode(ASTNodeType::BOOLEAN_LITERAL, token.start, token.end),
          value(token.kind == TokenKind::TRUE)
    {}
};


struct NullLiteralNode : ASTNode {
    explicit NullLiteralNode(const Token& token)
        : ASTNode(ASTNodeType::NULL_LITERAL, token.start, token.end)
    {}
};


struct IdentifierNode : ASTNode {
    std::string_view name;

    explicit IdentifierNode(const Token& token)
        : ASTNode(
            ASTNodeType::IDENTIFIER,
            token.start,
            token.end
        ),
          name(token.lexeme)
    {}

    IdentifierNode(std::string_view identifier_name, std::size_t start_position, std::size_t end_position)
        : ASTNode(ASTNodeType::IDENTIFIER, start_position, end_position),
          name(identifier_name) {}
};


struct ThisExprNode : ASTNode {
    explicit ThisExprNode(const Token& token)
        : ASTNode(ASTNodeType::THIS_EXPR, token.start, token.end) {}
};


// ============================================================
// Unary expression
//
// Examples:
//   !x
//   ~x
//   -x
//   +x
// ============================================================

struct UnaryExprNode : ASTNode {
    TokenKind op;
    std::unique_ptr<ASTNode> argument;

    UnaryExprNode(
        const Token& op_token,
        std::unique_ptr<ASTNode> arg
    )
        : ASTNode(
            ASTNodeType::UNARY_EXPR,
            op_token.start,
            arg->end
        ),
          op(op_token.kind),
          argument(std::move(arg))
    {}
};


// ============================================================
// Update expression
//
// Examples:
//   ++x
//   --x
//   x++
//   x--
//
// prefix:
//   ++x -> true
//   x++ -> false
// ============================================================

struct UpdateExprNode : ASTNode {
    TokenKind op;
    bool prefix;
    std::unique_ptr<ASTNode> argument;

    UpdateExprNode(
        const Token& op_token,
        std::unique_ptr<ASTNode> arg,
        bool is_prefix
    )
        : ASTNode(
            ASTNodeType::UPDATE_EXPR,

            is_prefix
                ? op_token.start
                : arg->start,

            is_prefix
                ? arg->end
                : op_token.end
        ),
          op(op_token.kind),
          prefix(is_prefix),
          argument(std::move(arg))
    {}
};


// ============================================================
// Binary expression
//
// Examples:
//   a + b
//   a * b
//   a << b
//   a === b
//   a & b
// ============================================================

struct BinaryExprNode : ASTNode {
    TokenKind op;

    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;

    BinaryExprNode(
        TokenKind operator_kind,
        std::unique_ptr<ASTNode> l,
        std::unique_ptr<ASTNode> r
    )
        : ASTNode(
            ASTNodeType::BINARY_EXPR,
            l->start,
            r->end
        ),
          op(operator_kind),
          left(std::move(l)),
          right(std::move(r))
    {}
};


// ============================================================
// Logical expression
//
// Examples:
//   a && b
//   a || b
//
// Kept separate from BinaryExpression because logical
// expressions eventually have short-circuit CFG semantics.
// ============================================================

struct LogicalExprNode : ASTNode {
    TokenKind op;

    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;

    LogicalExprNode(
        TokenKind operator_kind,
        std::unique_ptr<ASTNode> l,
        std::unique_ptr<ASTNode> r
    )
        : ASTNode(
            ASTNodeType::LOGICAL_EXPR,
            l->start,
            r->end
        ),
          op(operator_kind),
          left(std::move(l)),
          right(std::move(r))
    {}
};




// ============================================================
// Conditional expression
//
// Example:
//   test ? consequent : alternate
// ============================================================

struct ConditionalExprNode : ASTNode {
    std::unique_ptr<ASTNode> test;
    std::unique_ptr<ASTNode> consequent;
    std::unique_ptr<ASTNode> alternate;

    ConditionalExprNode(
        std::unique_ptr<ASTNode> test_node,
        std::unique_ptr<ASTNode> consequent_node,
        std::unique_ptr<ASTNode> alternate_node
    )
        : ASTNode(
            ASTNodeType::CONDITIONAL_EXPR,
            test_node->start,
            alternate_node->end
        ),
          test(std::move(test_node)),
          consequent(std::move(consequent_node)),
          alternate(std::move(alternate_node))
    {}
};


// ============================================================
// Sequence expression
//
// Example:
//   a, b, c
// ============================================================

struct SequenceExprNode : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> expressions;

    explicit SequenceExprNode(std::vector<std::unique_ptr<ASTNode>> items)
        : ASTNode(
            ASTNodeType::SEQUENCE_EXPR,
            items.front()->start,
            items.back()->end
        ),
          expressions(std::move(items))
    {}
};

// ============================================================
// Call expression
//
// foo()
// foo(1, 2)
// foo().bar(1)
//
// end_position must include the closing ')'
// ============================================================

struct CallExprNode : ASTNode {
    std::unique_ptr<ASTNode> callee;

    std::vector<std::unique_ptr<ASTNode>> arguments;

    CallExprNode(
        std::unique_ptr<ASTNode> callee_node,
        std::vector<std::unique_ptr<ASTNode>> args,
        std::size_t end_position
    )
        : ASTNode(
            ASTNodeType::CALL_EXPR,
            callee_node->start,
            end_position
        ),
          callee(std::move(callee_node)),
          arguments(std::move(args))
    {}
};


struct NewExprNode : ASTNode {
    std::unique_ptr<ASTNode> callee;
    std::vector<std::unique_ptr<ASTNode>> arguments;

    NewExprNode(
        std::unique_ptr<ASTNode> callee_node,
        std::vector<std::unique_ptr<ASTNode>> args,
        std::size_t end_position
    )
        : ASTNode(ASTNodeType::NEW_EXPR, callee_node->start, end_position),
          callee(std::move(callee_node)),
          arguments(std::move(args))
    {}
};


// ============================================================
// Spread element
// ============================================================

struct SpreadElementNode : ASTNode {
    std::unique_ptr<ASTNode> argument;

    SpreadElementNode(std::size_t start_position, std::unique_ptr<ASTNode> arg)
        : ASTNode(ASTNodeType::SPREAD_ELEMENT, start_position, arg->end),
          argument(std::move(arg)) {}
};


// ============================================================
// Member expression
//
// obj.foo
// obj[index]
//
// computed:
//   obj.foo    -> false
//   obj[index] -> true
//
// end_position:
//   obj.foo    -> property.end
//   obj[index] -> closing ']'.end
// ============================================================

struct MemberExprNode : ASTNode {
    std::unique_ptr<ASTNode> object;
    std::unique_ptr<ASTNode> property;

    bool computed;

    MemberExprNode(
        std::unique_ptr<ASTNode> object_node,
        std::unique_ptr<ASTNode> property_node,
        bool is_computed,
        std::size_t end_position
    )
        : ASTNode(
            ASTNodeType::MEMBER_EXPR,
            object_node->start,
            end_position
        ),
          object(std::move(object_node)),
          property(std::move(property_node)),
          computed(is_computed)
    {}
};




// ============================================================
// Optional chain
// ============================================================

enum class OptionalChainSegmentKind {
    STATIC_PROPERTY,
    COMPUTED_PROPERTY,
    CALL
};

struct OptionalChainSegment {
    OptionalChainSegmentKind kind{OptionalChainSegmentKind::STATIC_PROPERTY};
    bool optional{false};
    std::unique_ptr<ASTNode> property;
    std::vector<std::unique_ptr<ASTNode>> arguments;
    std::size_t end{0};
};

struct OptionalChainExprNode : ASTNode {
    std::unique_ptr<ASTNode> base;
    std::vector<OptionalChainSegment> segments;

    OptionalChainExprNode(
        std::unique_ptr<ASTNode> base_node,
        std::vector<OptionalChainSegment> chain_segments,
        std::size_t end_position)
        : ASTNode(ASTNodeType::OPTIONAL_CHAIN_EXPR, base_node->start, end_position),
          base(std::move(base_node)),
          segments(std::move(chain_segments)) {}
};


// ============================================================
// Assignment expression
//
// x = 1
// obj.x = 1
//
// op is retained even though we currently only parse '='.
// That avoids redesign when compound assignments arrive later.
// ============================================================

struct AssignmentExprNode : ASTNode {
    TokenKind op;

    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;

    AssignmentExprNode(
        TokenKind operator_kind,
        std::unique_ptr<ASTNode> l,
        std::unique_ptr<ASTNode> r
    )
        : ASTNode(
            ASTNodeType::ASSIGNMENT_EXPR,
            l->start,
            r->end
        ),
          op(operator_kind),
          left(std::move(l)),
          right(std::move(r))
    {}
};




// ============================================================
// P10 binding / assignment patterns
// ============================================================

struct RestElementNode : ASTNode {
    std::unique_ptr<ASTNode> argument;

    RestElementNode(std::size_t start_position, std::unique_ptr<ASTNode> target)
        : ASTNode(ASTNodeType::REST_ELEMENT, start_position, target->end),
          argument(std::move(target)) {}
};

struct AssignmentPatternNode : ASTNode {
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> right;

    AssignmentPatternNode(std::unique_ptr<ASTNode> target, std::unique_ptr<ASTNode> initializer)
        : ASTNode(ASTNodeType::ASSIGNMENT_PATTERN, target->start, initializer->end),
          left(std::move(target)), right(std::move(initializer)) {}
};

struct ArrayPatternNode : ASTNode {
    // nullptr entries are elisions and still consume one iterator element.
    std::vector<std::unique_ptr<ASTNode>> elements;

    ArrayPatternNode(const Token& left_bracket, const Token& right_bracket,
                     std::vector<std::unique_ptr<ASTNode>> pattern_elements)
        : ASTNode(ASTNodeType::ARRAY_PATTERN, left_bracket.start, right_bracket.end),
          elements(std::move(pattern_elements)) {}

    ArrayPatternNode(std::size_t start_position, std::size_t end_position,
                     std::vector<std::unique_ptr<ASTNode>> pattern_elements)
        : ASTNode(ASTNodeType::ARRAY_PATTERN, start_position, end_position),
          elements(std::move(pattern_elements)) {}
};

struct ObjectPatternPropertyNode : ASTNode {
    std::unique_ptr<ASTNode> key;
    std::unique_ptr<ASTNode> value;
    bool computed;

    ObjectPatternPropertyNode(std::unique_ptr<ASTNode> property_key,
                              std::unique_ptr<ASTNode> target,
                              bool is_computed = false)
        : ASTNode(ASTNodeType::OBJECT_PATTERN_PROPERTY, property_key->start, target->end),
          key(std::move(property_key)), value(std::move(target)), computed(is_computed) {}
};

struct ObjectPatternNode : ASTNode {
    std::vector<std::unique_ptr<ObjectPatternPropertyNode>> properties;
    std::unique_ptr<RestElementNode> rest;

    ObjectPatternNode(const Token& left_brace, const Token& right_brace,
                      std::vector<std::unique_ptr<ObjectPatternPropertyNode>> pattern_properties,
                      std::unique_ptr<RestElementNode> rest_element = nullptr)
        : ASTNode(ASTNodeType::OBJECT_PATTERN, left_brace.start, right_brace.end),
          properties(std::move(pattern_properties)), rest(std::move(rest_element)) {}

    ObjectPatternNode(std::size_t start_position, std::size_t end_position,
                      std::vector<std::unique_ptr<ObjectPatternPropertyNode>> pattern_properties,
                      std::unique_ptr<RestElementNode> rest_element = nullptr)
        : ASTNode(ASTNodeType::OBJECT_PATTERN, start_position, end_position),
          properties(std::move(pattern_properties)), rest(std::move(rest_element)) {}
};


// ============================================================
// Expression statement
//
// foo();
// x = 10;
//
// statement_end allows the range to include ';'
// ============================================================

struct ExpressionStatementNode : ASTNode {
    std::unique_ptr<ASTNode> expression;

    ExpressionStatementNode(
        std::unique_ptr<ASTNode> expr,
        std::size_t statement_end
    )
        : ASTNode(
            ASTNodeType::EXPRESSION_STATEMENT,
            expr->start,
            statement_end
        ),
          expression(std::move(expr))
    {}
};


// ============================================================
// Empty statement
//
// ;
// ============================================================

struct EmptyStatementNode : ASTNode {

    explicit EmptyStatementNode(const Token& semicolon)
        : ASTNode(
            ASTNodeType::EMPTY_STATEMENT,
            semicolon.start,
            semicolon.end
        )
    {}
};


// ============================================================
// Block statement
//
// {
//     statement;
//     statement;
// }
// ============================================================

struct BlockStatementNode : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> body;

    BlockStatementNode(
        const Token& left_brace,
        const Token& right_brace,
        std::vector<std::unique_ptr<ASTNode>> statements
    )
        : ASTNode(
            ASTNodeType::BLOCK_STATEMENT,
            left_brace.start,
            right_brace.end
        ),
          body(std::move(statements))
    {}
};


// ============================================================
// Return statement
//
// return;
// return expression;
//
// argument == nullptr means:
//   return;
// ============================================================

struct ReturnStatementNode : ASTNode {
    std::unique_ptr<ASTNode> argument;

    ReturnStatementNode(
        const Token& return_token,
        std::unique_ptr<ASTNode> arg,
        std::size_t statement_end
    )
        : ASTNode(
            ASTNodeType::RETURN_STATEMENT,
            return_token.start,
            statement_end
        ),
          argument(std::move(arg))
    {}
};




struct ThrowStatementNode : ASTNode {
    std::unique_ptr<ASTNode> argument;

    ThrowStatementNode(const Token& throw_token, std::unique_ptr<ASTNode> arg, std::size_t statement_end)
        : ASTNode(ASTNodeType::THROW_STATEMENT, throw_token.start, statement_end),
          argument(std::move(arg)) {}
};

struct TryStatementNode : ASTNode {
    std::unique_ptr<BlockStatementNode> block;
    std::unique_ptr<ASTNode> handler_param;
    std::unique_ptr<BlockStatementNode> handler;
    std::unique_ptr<BlockStatementNode> finalizer;

    TryStatementNode(
        const Token& try_token,
        std::unique_ptr<BlockStatementNode> try_block,
        std::unique_ptr<ASTNode> catch_param,
        std::unique_ptr<BlockStatementNode> catch_block,
        std::unique_ptr<BlockStatementNode> finally_block)
        : ASTNode(
              ASTNodeType::TRY_STATEMENT,
              try_token.start,
              finally_block ? finally_block->end : (catch_block ? catch_block->end : try_block->end)),
          block(std::move(try_block)),
          handler_param(std::move(catch_param)),
          handler(std::move(catch_block)),
          finalizer(std::move(finally_block)) {}
};

// ============================================================
// Variable declarator
//
// let x = 10;
//     ^^^^^^
//
// id   -> Identifier(x)
// init -> NumberLiteral(10)
//
// init may be nullptr:
//
// let x;
// ============================================================

struct VariableDeclaratorNode : ASTNode {
    std::unique_ptr<ASTNode> id;
    std::unique_ptr<ASTNode> init;

    VariableDeclaratorNode(
        std::unique_ptr<ASTNode> identifier,
        std::unique_ptr<ASTNode> initializer
    )
        : ASTNode(
            ASTNodeType::VARIABLE_DECLARATOR,
            identifier->start,

            initializer
                ? initializer->end
                : identifier->end
        ),
          id(std::move(identifier)),
          init(std::move(initializer))
    {}
};


// ============================================================
// Variable declaration
//
// let x = 1;
// const x = 1, y = 2;
// var z;
//
// declaration_end should include ';' when present.
// ============================================================

struct VariableDeclarationNode : ASTNode {
    VariableKind kind;

    std::vector<std::unique_ptr<VariableDeclaratorNode>> declarations;

    VariableDeclarationNode(
        const Token& keyword_token,
        VariableKind variable_kind,
        std::vector<std::unique_ptr<VariableDeclaratorNode>> declarators,
        std::size_t declaration_end
    )
        : ASTNode(
            ASTNodeType::VARIABLE_DECLARATION,
            keyword_token.start,
            declaration_end
        ),
          kind(variable_kind),
          declarations(std::move(declarators))
    {}
};

// ===========================================================
// Function declaration 
// function outer(a, b) {
//   function inner(x, y) {
//        return x + y;
//    }
//   return inner(a,b);
// }
// ===========================================================
struct FunctionDeclarationNode : ASTNode {
    std::unique_ptr<IdentifierNode> id;
    std::vector<std::unique_ptr<ASTNode>> params;
    std::vector<std::unique_ptr<ASTNode>> param_defaults;
    std::optional<std::size_t> rest_parameter;
    std::unique_ptr<BlockStatementNode> body;

    bool async;
    bool generator;

    FunctionDeclarationNode(
        const Token& function_token,
        std::unique_ptr<IdentifierNode> identifier,
        std::vector<std::unique_ptr<ASTNode>> parameters,
        std::unique_ptr<BlockStatementNode> function_body,
        bool is_async = false,
        bool is_generator = false
    )
        : ASTNode(
            ASTNodeType::FUNCTION_DECLARATION,
            function_token.start,
            function_body->end
        ),
          id(std::move(identifier)),
          params(std::move(parameters)),
          param_defaults(params.size()),
          body(std::move(function_body)),
          async(is_async),
          generator(is_generator)
    {}
};

struct FunctionExpressionNode : ASTNode {
    // FunctionExpression names are optional.  A present name is scoped only
    // inside the function body; FunctionDeclaration keeps its required id.
    std::unique_ptr<IdentifierNode> id;
    std::vector<std::unique_ptr<ASTNode>> params;
    std::vector<std::unique_ptr<ASTNode>> param_defaults;
    std::optional<std::size_t> rest_parameter;
    std::unique_ptr<BlockStatementNode> body;
    bool async;
    bool generator;

    FunctionExpressionNode(
        const Token& function_token,
        std::unique_ptr<IdentifierNode> identifier,
        std::vector<std::unique_ptr<ASTNode>> parameters,
        std::unique_ptr<BlockStatementNode> function_body,
        bool is_async = false,
        bool is_generator = false
    )
        : ASTNode(ASTNodeType::FUNCTION_EXPR, function_token.start, function_body->end),
          id(std::move(identifier)),
          params(std::move(parameters)),
          param_defaults(params.size()),
          body(std::move(function_body)),
          async(is_async),
          generator(is_generator)
    {}
};

// ===========================================================
// Control flow
// ===========================================================
struct IfStatementNode : ASTNode {
    std::unique_ptr<ASTNode> test;
    std::unique_ptr<ASTNode> consequent;
    std::unique_ptr<ASTNode> alternate;

    IfStatementNode(
        const Token& if_token,
        std::unique_ptr<ASTNode> condition,
        std::unique_ptr<ASTNode> consequent_node,
        std::unique_ptr<ASTNode> alternate_node
    )
        : ASTNode(
            ASTNodeType::IF_STATEMENT,
            if_token.start,
            alternate_node
                ? alternate_node->end
                : consequent_node->end
        ),
          test(std::move(condition)),
          consequent(std::move(consequent_node)),
          alternate(std::move(alternate_node))
    {}
};

struct WhileStatementNode : ASTNode {
    std::unique_ptr<ASTNode> test;
    std::unique_ptr<ASTNode> body;

    WhileStatementNode(
        const Token& while_token,
        std::unique_ptr<ASTNode> condition,
        std::unique_ptr<ASTNode> loop_body
    )
        : ASTNode(
            ASTNodeType::WHILE_STATEMENT,
            while_token.start,
            loop_body->end
        ),
          test(std::move(condition)),
          body(std::move(loop_body))
    {}
};

struct DoWhileStatementNode : ASTNode {
    std::unique_ptr<ASTNode> body;
    std::unique_ptr<ASTNode> test;

    DoWhileStatementNode(const Token& do_token, std::unique_ptr<ASTNode> loop_body,
                         std::unique_ptr<ASTNode> condition, std::size_t statement_end)
        : ASTNode(ASTNodeType::DO_WHILE_STATEMENT, do_token.start, statement_end),
          body(std::move(loop_body)), test(std::move(condition)) {}
};

struct ForStatementNode : ASTNode {
    std::unique_ptr<ASTNode> init;
    std::unique_ptr<ASTNode> test;
    std::unique_ptr<ASTNode> update;
    std::unique_ptr<ASTNode> body;

    ForStatementNode(const Token& for_token, std::unique_ptr<ASTNode> initializer,
                     std::unique_ptr<ASTNode> condition, std::unique_ptr<ASTNode> increment,
                     std::unique_ptr<ASTNode> loop_body)
        : ASTNode(ASTNodeType::FOR_STATEMENT, for_token.start, loop_body->end),
          init(std::move(initializer)), test(std::move(condition)),
          update(std::move(increment)), body(std::move(loop_body)) {}
};

struct ForInStatementNode : ASTNode {
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> object;
    std::unique_ptr<ASTNode> body;

    ForInStatementNode(const Token& for_token, std::unique_ptr<ASTNode> lhs,
                       std::unique_ptr<ASTNode> rhs, std::unique_ptr<ASTNode> loop_body)
        : ASTNode(ASTNodeType::FOR_IN_STATEMENT, for_token.start, loop_body->end),
          left(std::move(lhs)), object(std::move(rhs)), body(std::move(loop_body)) {}
};

struct ForOfStatementNode : ASTNode {
    std::unique_ptr<ASTNode> left;
    std::unique_ptr<ASTNode> iterable;
    std::unique_ptr<ASTNode> body;

    ForOfStatementNode(const Token& for_token, std::unique_ptr<ASTNode> lhs,
                       std::unique_ptr<ASTNode> iterable_expression,
                       std::unique_ptr<ASTNode> loop_body)
        : ASTNode(ASTNodeType::FOR_OF_STATEMENT, for_token.start, loop_body->end),
          left(std::move(lhs)), iterable(std::move(iterable_expression)),
          body(std::move(loop_body)) {}
};

struct SwitchCaseNode {
    std::unique_ptr<ASTNode> test; // null => default
    std::vector<std::unique_ptr<ASTNode>> consequent;
    std::size_t start{0};
    std::size_t end{0};
};

struct SwitchStatementNode : ASTNode {
    std::unique_ptr<ASTNode> discriminant;
    std::vector<SwitchCaseNode> cases;

    SwitchStatementNode(const Token& switch_token, std::unique_ptr<ASTNode> value,
                        std::vector<SwitchCaseNode> clauses, std::size_t statement_end)
        : ASTNode(ASTNodeType::SWITCH_STATEMENT, switch_token.start, statement_end),
          discriminant(std::move(value)), cases(std::move(clauses)) {}
};

struct LabeledStatementNode : ASTNode {
    std::string label;
    std::unique_ptr<ASTNode> body;

    LabeledStatementNode(const Token& label_token, std::unique_ptr<ASTNode> statement)
        : ASTNode(ASTNodeType::LABELED_STATEMENT, label_token.start, statement->end),
          label(label_token.lexeme), body(std::move(statement)) {}
};

struct BreakStatementNode : ASTNode {
    std::optional<std::string> label;
    BreakStatementNode(const Token& break_token, std::optional<std::string> target,
                       std::size_t statement_end)
        : ASTNode(ASTNodeType::BREAK_STATEMENT, break_token.start, statement_end),
          label(std::move(target)) {}
};

struct ContinueStatementNode : ASTNode {
    std::optional<std::string> label;
    ContinueStatementNode(const Token& continue_token, std::optional<std::string> target,
                          std::size_t statement_end)
        : ASTNode(ASTNodeType::CONTINUE_STATEMENT, continue_token.start, statement_end),
          label(std::move(target)) {}
};

struct DebuggerStatementNode : ASTNode {
    DebuggerStatementNode(const Token& debugger_token, std::size_t statement_end)
        : ASTNode(ASTNodeType::DEBUGGER_STATEMENT, debugger_token.start, statement_end) {}
};


// ============================================================
// Array expression
// ============================================================

struct ArrayExprNode : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> elements;

    ArrayExprNode(
        const Token& left_bracket,
        const Token& right_bracket,
        std::vector<std::unique_ptr<ASTNode>> array_elements
    )
        : ASTNode(
            ASTNodeType::ARRAY_EXPR,
            left_bracket.start,
            right_bracket.end
        ),
          elements(std::move(array_elements))
    {}
};


// ============================================================
// Object property / object expression
// ============================================================

enum class PropertyKind { Data, Getter, Setter };

struct PropertyNode : ASTNode {
    std::unique_ptr<ASTNode> key;
    std::unique_ptr<ASTNode> value;

    bool shorthand;
    bool computed;
    PropertyKind kind;

    PropertyNode(
        std::unique_ptr<ASTNode> property_key,
        std::unique_ptr<ASTNode> property_value,
        bool is_shorthand = false,
        bool is_computed = false,
        PropertyKind property_kind = PropertyKind::Data
    )
        : ASTNode(
            ASTNodeType::PROPERTY,
            property_key->start,
            property_value->end
        ),
          key(std::move(property_key)),
          value(std::move(property_value)),
          shorthand(is_shorthand),
          computed(is_computed),
          kind(property_kind)
    {}
};

struct ObjectExprNode : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> properties;

    ObjectExprNode(
        const Token& left_brace,
        const Token& right_brace,
        std::vector<std::unique_ptr<ASTNode>> object_properties
    )
        : ASTNode(
            ASTNodeType::OBJECT_EXPR,
            left_brace.start,
            right_brace.end
        ),
          properties(std::move(object_properties))
    {}
};


// ============================================================
// Arrow function expression
// ============================================================

struct ArrowFunctionExprNode : ASTNode {
    std::vector<std::unique_ptr<ASTNode>> params;
    std::vector<std::unique_ptr<ASTNode>> param_defaults;
    std::optional<std::size_t> rest_parameter;
    std::unique_ptr<ASTNode> body;

    bool expression_body;
    bool async;

    ArrowFunctionExprNode(
        std::size_t start_position,
        std::vector<std::unique_ptr<ASTNode>> parameters,
        std::unique_ptr<ASTNode> function_body,
        bool has_expression_body,
        bool is_async = false
    )
        : ASTNode(
            ASTNodeType::ARROW_FUNCTION_EXPR,
            start_position,
            function_body->end
        ),
          params(std::move(parameters)),
          param_defaults(params.size()),
          body(std::move(function_body)),
          expression_body(has_expression_body),
          async(is_async)
    {}
};


// ============================================================
// RegExp literal
// ============================================================

struct RegExpLiteralNode : ASTNode {
    std::string_view pattern;
    std::string_view flags;

    RegExpLiteralNode(
        std::size_t start_position,
        std::size_t end_position,
        std::string_view regexp_pattern,
        std::string_view regexp_flags
    )
        : ASTNode(
            ASTNodeType::REGEXP_LITERAL,
            start_position,
            end_position
        ),
          pattern(regexp_pattern),
          flags(regexp_flags)
    {}
};


// ============================================================
// Await / yield expressions
// ============================================================

struct AwaitExprNode : ASTNode {
    std::unique_ptr<ASTNode> argument;

    AwaitExprNode(
        const Token& await_token,
        std::unique_ptr<ASTNode> arg
    )
        : ASTNode(
            ASTNodeType::AWAIT_EXPR,
            await_token.start,
            arg->end
        ),
          argument(std::move(arg))
    {}
};

struct YieldExprNode : ASTNode {
    std::unique_ptr<ASTNode> argument;

    YieldExprNode(
        const Token& yield_token,
        std::unique_ptr<ASTNode> arg
    )
        : ASTNode(
            ASTNodeType::YIELD_EXPR,
            yield_token.start,
            arg ? arg->end : yield_token.end
        ),
          argument(std::move(arg))
    {}
};


// ============================================================
// Template literal
// ============================================================

struct TemplateElementNode : ASTNode {
    std::string raw;
    std::string cooked;
    bool tail;

    TemplateElementNode(
        std::size_t start_position,
        std::size_t end_position,
        std::string raw_value,
        std::string cooked_value,
        bool is_tail
    )
        : ASTNode(ASTNodeType::TEMPLATE_ELEMENT, start_position, end_position),
          raw(std::move(raw_value)), cooked(std::move(cooked_value)), tail(is_tail)
    {}
};

struct TemplateLiteralNode : ASTNode {
    std::vector<std::unique_ptr<TemplateElementNode>> quasis;
    std::vector<std::unique_ptr<ASTNode>> expressions;

    TemplateLiteralNode(
        std::size_t start_position,
        std::size_t end_position,
        std::vector<std::unique_ptr<TemplateElementNode>> template_quasis,
        std::vector<std::unique_ptr<ASTNode>> template_expressions
    )
        : ASTNode(
            ASTNodeType::TEMPLATE_LITERAL,
            start_position,
            end_position
        ),
          quasis(std::move(template_quasis)),
          expressions(std::move(template_expressions))
    {}
};


struct TaggedTemplateExprNode : ASTNode {
    std::unique_ptr<ASTNode> tag;
    std::unique_ptr<TemplateLiteralNode> quasi;

    TaggedTemplateExprNode(std::unique_ptr<ASTNode> tag_expression,
                           std::unique_ptr<TemplateLiteralNode> template_literal)
        : ASTNode(ASTNodeType::TAGGED_TEMPLATE_EXPR, tag_expression->start, template_literal->end),
          tag(std::move(tag_expression)), quasi(std::move(template_literal)) {}
};


// ============================================================
// Module syntax
// ============================================================

struct ImportSpecifierNode : ASTNode {
    std::unique_ptr<IdentifierNode> imported;
    std::unique_ptr<IdentifierNode> local;

    ImportSpecifierNode(
        std::unique_ptr<IdentifierNode> imported_name,
        std::unique_ptr<IdentifierNode> local_name
    )
        : ASTNode(
            ASTNodeType::IMPORT_SPECIFIER,
            imported_name->start,
            local_name->end
        ),
          imported(std::move(imported_name)),
          local(std::move(local_name))
    {}
};

struct ImportDeclarationNode : ASTNode {
    std::vector<std::unique_ptr<ImportSpecifierNode>> specifiers;
    std::unique_ptr<StringLiteralNode> source;

    ImportDeclarationNode(
        const Token& import_token,
        std::vector<std::unique_ptr<ImportSpecifierNode>> import_specifiers,
        std::unique_ptr<StringLiteralNode> import_source,
        std::size_t statement_end
    )
        : ASTNode(
            ASTNodeType::IMPORT_DECLARATION,
            import_token.start,
            statement_end
        ),
          specifiers(std::move(import_specifiers)),
          source(std::move(import_source))
    {}
};

struct ExportNamedDeclarationNode : ASTNode {
    std::unique_ptr<ASTNode> declaration;

    std::vector<std::unique_ptr<IdentifierNode>> specifiers;

    ExportNamedDeclarationNode(
        const Token& export_token,
        std::unique_ptr<ASTNode> exported_declaration,
        std::vector<std::unique_ptr<IdentifierNode>> export_specifiers,
        std::size_t end_position
    )
        : ASTNode(
            ASTNodeType::EXPORT_NAMED_DECLARATION,
            export_token.start,
            exported_declaration ? exported_declaration->end : end_position
        ),
          declaration(std::move(exported_declaration)),
          specifiers(std::move(export_specifiers))
    {}
};


// ============================================================
// Class syntax
// ============================================================

struct MethodDefinitionNode : ASTNode {
    std::unique_ptr<IdentifierNode> key;
    std::vector<std::unique_ptr<ASTNode>> params;
    std::vector<std::unique_ptr<ASTNode>> param_defaults;
    std::optional<std::size_t> rest_parameter;
    std::unique_ptr<BlockStatementNode> body;

    bool async;
    bool generator;

    MethodDefinitionNode(
        std::unique_ptr<IdentifierNode> method_key,
        std::vector<std::unique_ptr<ASTNode>> parameters,
        std::unique_ptr<BlockStatementNode> method_body,
        bool is_async = false,
        bool is_generator = false
    )
        : ASTNode(
            ASTNodeType::METHOD_DEFINITION,
            method_key->start,
            method_body->end
        ),
          key(std::move(method_key)),
          params(std::move(parameters)),
          param_defaults(params.size()),
          body(std::move(method_body)),
          async(is_async),
          generator(is_generator)
    {}
};

struct ClassDeclarationNode : ASTNode {
    std::unique_ptr<IdentifierNode> id;
    std::unique_ptr<ASTNode> super_class;
    std::vector<std::unique_ptr<MethodDefinitionNode>> methods;

    ClassDeclarationNode(
        const Token& class_token,
        std::unique_ptr<IdentifierNode> identifier,
        std::unique_ptr<ASTNode> parent_class,
        std::vector<std::unique_ptr<MethodDefinitionNode>> class_methods,
        std::size_t class_end
    )
        : ASTNode(
            ASTNodeType::CLASS_DECLARATION,
            class_token.start,
            class_end
        ),
          id(std::move(identifier)),
          super_class(std::move(parent_class)),
          methods(std::move(class_methods))
    {}
};

// ============================================================
// Printing helpers
// ============================================================

inline const char* op_to_string(TokenKind op) {
    switch (op) {

        // Arithmetic
        case TokenKind::PLUS:
            return "+";

        case TokenKind::MINUS:
            return "-";

        case TokenKind::STAR:
            return "*";

        case TokenKind::STAR_STAR:
            return "**";

        case TokenKind::SLASH:
            return "/";

        case TokenKind::PERCENT:
            return "%";


        // Update
        case TokenKind::PLUS_PLUS:
            return "++";

        case TokenKind::MINUS_MINUS:
            return "--";


        // Unary
        case TokenKind::BANG:
            return "!";

        case TokenKind::TILDE:
            return "~";

        case TokenKind::DELETE:
            return "delete";

        case TokenKind::VOID:
            return "void";

        case TokenKind::TYPEOF:
            return "typeof";


        // Relational
        case TokenKind::LESS:
            return "<";

        case TokenKind::LESS_EQUAL:
            return "<=";

        case TokenKind::GREATER:
            return ">";

        case TokenKind::GREATER_EQUAL:
            return ">=";

        case TokenKind::INSTANCEOF:
            return "instanceof";

        case TokenKind::IN:
            return "in";


        // Equality
        case TokenKind::EQUAL_EQUAL:
            return "==";

        case TokenKind::BANG_EQUAL:
            return "!=";

        case TokenKind::EQUAL_EQUAL_EQUAL:
            return "===";

        case TokenKind::BANG_EQUAL_EQUAL:
            return "!==";


        // Shifts
        case TokenKind::SHIFT_LEFT:
            return "<<";

        case TokenKind::SHIFT_RIGHT:
            return ">>";

        case TokenKind::SHIFT_RIGHT_UNSIGNED:
            return ">>>";


        // Bitwise
        case TokenKind::AMPERSAND:
            return "&";

        case TokenKind::PIPE:
            return "|";

        case TokenKind::CARET:
            return "^";


        // Logical
        case TokenKind::AND_AND:
            return "&&";

        case TokenKind::OR_OR:
            return "||";

        case TokenKind::NULLISH:
            return "??";


        // Assignment
        case TokenKind::EQUAL:
            return "=";
        case TokenKind::PLUS_EQUAL:
            return "+=";
        case TokenKind::MINUS_EQUAL:
            return "-=";
        case TokenKind::STAR_EQUAL:
            return "*=";
        case TokenKind::STAR_STAR_EQUAL:
            return "**=";
        case TokenKind::SLASH_EQUAL:
            return "/=";
        case TokenKind::PERCENT_EQUAL:
            return "%=";
        case TokenKind::SHIFT_LEFT_EQUAL:
            return "<<=";
        case TokenKind::SHIFT_RIGHT_EQUAL:
            return ">>=";
        case TokenKind::SHIFT_RIGHT_UNSIGNED_EQUAL:
            return ">>>=";
        case TokenKind::AMPERSAND_EQUAL:
            return "&=";
        case TokenKind::CARET_EQUAL:
            return "^=";
        case TokenKind::PIPE_EQUAL:
            return "|=";
        case TokenKind::AND_AND_EQUAL:
            return "&&=";
        case TokenKind::OR_OR_EQUAL:
            return "||=";
        case TokenKind::NULLISH_EQUAL:
            return "?" "?=";


        default:
            return "?";
    }
}


inline const char* variable_kind_to_string(VariableKind kind) {
    switch (kind) {
        case VariableKind::LET:
            return "let";

        case VariableKind::CONST:
            return "const";

        case VariableKind::VAR:
            return "var";
    }

    return "?";
}


// ============================================================
// AST printer
// ============================================================

inline void print_ast(
    const ASTNode* node,
    int depth = 0
) {
    if (!node) {
        return;
    }

    std::string indent(static_cast<std::size_t>(depth) * 2U, ' ');

    switch (node->type) {

        // ----------------------------------------------------
        // Program
        // ----------------------------------------------------

        case ASTNodeType::PROGRAM: {
            auto* program =
                static_cast<const ProgramNode*>(node);

            std::cout
                << indent
                << "Program"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            for (const auto& statement : program->body) {
                print_ast(statement.get(), depth + 1);
            }

            break;
        }


        // ----------------------------------------------------
        // Leaves
        // ----------------------------------------------------

        case ASTNodeType::NUMBER_LITERAL: {
            auto* number =
                static_cast<const NumberLiteralNode*>(node);

            std::cout
                << indent
                << "NumberLiteral("
                << number->value
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            break;
        }


        case ASTNodeType::STRING_LITERAL: {
            auto* string =
                static_cast<const StringLiteralNode*>(node);

            std::cout
                << indent
                << "StringLiteral("
                << string->value
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            break;
        }


        case ASTNodeType::BOOLEAN_LITERAL: {
            auto* literal = static_cast<const BooleanLiteralNode*>(node);
            std::cout << indent << "BooleanLiteral(" << (literal->value ? "true" : "false")
                      << ") [" << node->start << ", " << node->end << ")\n";
            break;
        }
        case ASTNodeType::NULL_LITERAL:
            std::cout << indent << "NullLiteral [" << node->start << ", " << node->end << ")\n";
            break;

        case ASTNodeType::IDENTIFIER: {
            auto* identifier =
                static_cast<const IdentifierNode*>(node);

            std::cout
                << indent
                << "Identifier("
                << identifier->name
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            break;
        }


        // ----------------------------------------------------
        // Expressions
        // ----------------------------------------------------

        case ASTNodeType::THIS_EXPR: {
            std::cout
                << indent
                << "ThisExpression"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";
            break;
        }


        case ASTNodeType::UNARY_EXPR: {
            auto* unary =
                static_cast<const UnaryExprNode*>(node);

            std::cout
                << indent
                << "UnaryExpr("
                << op_to_string(unary->op)
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(
                unary->argument.get(),
                depth + 1
            );

            break;
        }


        case ASTNodeType::UPDATE_EXPR: {
            auto* update =
                static_cast<const UpdateExprNode*>(node);

            std::cout
                << indent
                << "UpdateExpr("
                << op_to_string(update->op)
                << ", "
                << (update->prefix ? "prefix" : "postfix")
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(
                update->argument.get(),
                depth + 1
            );

            break;
        }


        case ASTNodeType::BINARY_EXPR: {
            auto* binary =
                static_cast<const BinaryExprNode*>(node);

            std::cout
                << indent
                << "BinaryExpr("
                << op_to_string(binary->op)
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(
                binary->left.get(),
                depth + 1
            );

            print_ast(
                binary->right.get(),
                depth + 1
            );

            break;
        }


        case ASTNodeType::LOGICAL_EXPR: {
            auto* logical =
                static_cast<const LogicalExprNode*>(node);

            std::cout
                << indent
                << "LogicalExpr("
                << op_to_string(logical->op)
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(
                logical->left.get(),
                depth + 1
            );

            print_ast(
                logical->right.get(),
                depth + 1
            );

            break;
        }


        case ASTNodeType::CONDITIONAL_EXPR: {
            auto* conditional =
                static_cast<const ConditionalExprNode*>(node);

            std::cout
                << indent
                << "ConditionalExpr"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(conditional->test.get(), depth + 1);
            print_ast(conditional->consequent.get(), depth + 1);
            print_ast(conditional->alternate.get(), depth + 1);
            break;
        }


        case ASTNodeType::SEQUENCE_EXPR: {
            auto* sequence =
                static_cast<const SequenceExprNode*>(node);

            std::cout
                << indent
                << "SequenceExpr"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            for (const auto& expression : sequence->expressions) {
                print_ast(expression.get(), depth + 1);
            }
            break;
        }


        case ASTNodeType::CALL_EXPR: {
            auto* call =
                static_cast<const CallExprNode*>(node);

            std::cout
                << indent
                << "CallExpr"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout
                << indent
                << "  Callee:\n";

            print_ast(
                call->callee.get(),
                depth + 2
            );

            if (!call->arguments.empty()) {
                std::cout
                    << indent
                    << "  Arguments:\n";

                for (const auto& argument : call->arguments) {
                    print_ast(
                        argument.get(),
                        depth + 2
                    );
                }
            }

            break;
        }

        case ASTNodeType::NEW_EXPR: {
            auto* expression = static_cast<const NewExprNode*>(node);
            std::cout << indent << "NewExpr [" << node->start << ", " << node->end << ")\n";
            std::cout << indent << "  Callee:\n";
            print_ast(expression->callee.get(), depth + 2);
            if (!expression->arguments.empty()) {
                std::cout << indent << "  Arguments:\n";
                for (const auto& argument : expression->arguments) print_ast(argument.get(), depth + 2);
            }
            break;
        }


        case ASTNodeType::MEMBER_EXPR: {
            auto* member =
                static_cast<const MemberExprNode*>(node);

            std::cout
                << indent
                << "MemberExpr("
                << (member->computed ? "computed" : "direct")
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout
                << indent
                << "  Object:\n";

            print_ast(
                member->object.get(),
                depth + 2
            );

            std::cout
                << indent
                << "  Property:\n";

            print_ast(
                member->property.get(),
                depth + 2
            );

            break;
        }


        case ASTNodeType::ASSIGNMENT_EXPR: {
            auto* assignment =
                static_cast<const AssignmentExprNode*>(node);

            std::cout
                << indent
                << "AssignmentExpr("
                << op_to_string(assignment->op)
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(
                assignment->left.get(),
                depth + 1
            );

            print_ast(
                assignment->right.get(),
                depth + 1
            );

            break;
        }



        case ASTNodeType::SPREAD_ELEMENT: {
            auto* spread = static_cast<const SpreadElementNode*>(node);
            std::cout << indent << "SpreadElement\n";
            print_ast(spread->argument.get(), depth + 1);
            break;
        }
        case ASTNodeType::TAGGED_TEMPLATE_EXPR: {
            auto* tagged = static_cast<const TaggedTemplateExprNode*>(node);
            std::cout << indent << "TaggedTemplate\n";
            print_ast(tagged->tag.get(), depth + 1);
            print_ast(tagged->quasi.get(), depth + 1);
            break;
        }
        case ASTNodeType::OPTIONAL_CHAIN_EXPR: {
            auto* chain = static_cast<const OptionalChainExprNode*>(node);
            std::cout << indent << "OptionalChain\n";
            print_ast(chain->base.get(), depth + 1);
            for (const auto& segment : chain->segments) {
                std::cout << indent << "  Segment" << (segment.optional ? " optional" : "") << "\n";
                if (segment.property) print_ast(segment.property.get(), depth + 2);
                for (const auto& argument : segment.arguments) print_ast(argument.get(), depth + 2);
            }
            break;
        }
        case ASTNodeType::ARRAY_EXPR: {
            auto* array = static_cast<const ArrayExprNode*>(node);

            std::cout
                << indent
                << "ArrayExpr"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            for (const auto& element : array->elements) {
                print_ast(element.get(), depth + 1);
            }

            break;
        }

        case ASTNodeType::PROPERTY: {
            auto* property = static_cast<const PropertyNode*>(node);

            std::cout
                << indent
                << "Property("
                << (property->shorthand ? "shorthand" : "key-value")
                << ", "
                << (property->computed ? "computed" : "direct")
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout << indent << "  Key:\n";
            print_ast(property->key.get(), depth + 2);

            std::cout << indent << "  Value:\n";
            print_ast(property->value.get(), depth + 2);

            break;
        }

        case ASTNodeType::OBJECT_EXPR: {
            auto* object = static_cast<const ObjectExprNode*>(node);

            std::cout
                << indent
                << "ObjectExpr"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            for (const auto& property : object->properties) {
                print_ast(property.get(), depth + 1);
            }

            break;
        }

        case ASTNodeType::ARROW_FUNCTION_EXPR: {
            auto* arrow = static_cast<const ArrowFunctionExprNode*>(node);

            std::cout
                << indent
                << "ArrowFunctionExpr("
                << (arrow->async ? "async, " : "")
                << (arrow->expression_body ? "expression-body" : "block-body")
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout << indent << "  Params:\n";
            for (const auto& param : arrow->params) {
                print_ast(param.get(), depth + 2);
            }

            std::cout << indent << "  Body:\n";
            print_ast(arrow->body.get(), depth + 2);

            break;
        }

        case ASTNodeType::REGEXP_LITERAL: {
            auto* regexp = static_cast<const RegExpLiteralNode*>(node);

            std::cout
                << indent
                << "RegExpLiteral(/"
                << regexp->pattern
                << "/"
                << regexp->flags
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            break;
        }

        case ASTNodeType::AWAIT_EXPR: {
            auto* await_expr = static_cast<const AwaitExprNode*>(node);

            std::cout
                << indent
                << "AwaitExpr"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(await_expr->argument.get(), depth + 1);
            break;
        }

        case ASTNodeType::YIELD_EXPR: {
            auto* yield_expr = static_cast<const YieldExprNode*>(node);

            std::cout
                << indent
                << "YieldExpr"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            if (yield_expr->argument) {
                print_ast(yield_expr->argument.get(), depth + 1);
            }

            break;
        }

        case ASTNodeType::TEMPLATE_ELEMENT: {
            auto* element = static_cast<const TemplateElementNode*>(node);

            std::cout
                << indent
                << "TemplateElement("
                << element->cooked
                << ", "
                << (element->tail ? "tail" : "middle")
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            break;
        }

        case ASTNodeType::TEMPLATE_LITERAL: {
            auto* literal = static_cast<const TemplateLiteralNode*>(node);

            std::cout
                << indent
                << "TemplateLiteral"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout << indent << "  Quasis:\n";
            for (const auto& quasi : literal->quasis) {
                print_ast(quasi.get(), depth + 2);
            }

            if (!literal->expressions.empty()) {
                std::cout << indent << "  Expressions:\n";
                for (const auto& expression : literal->expressions) {
                    print_ast(expression.get(), depth + 2);
                }
            }

            break;
        }

        // ----------------------------------------------------
        // Statements
        // ----------------------------------------------------

        case ASTNodeType::EXPRESSION_STATEMENT: {
            auto* statement =
                static_cast<const ExpressionStatementNode*>(node);

            std::cout
                << indent
                << "ExpressionStatement"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(
                statement->expression.get(),
                depth + 1
            );

            break;
        }


        case ASTNodeType::EMPTY_STATEMENT: {
            std::cout
                << indent
                << "EmptyStatement"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            break;
        }


        case ASTNodeType::BLOCK_STATEMENT: {
            auto* block =
                static_cast<const BlockStatementNode*>(node);

            std::cout
                << indent
                << "BlockStatement"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            for (const auto& statement : block->body) {
                print_ast(
                    statement.get(),
                    depth + 1
                );
            }

            break;
        }


        case ASTNodeType::RETURN_STATEMENT: {
            auto* return_statement =
                static_cast<const ReturnStatementNode*>(node);

            std::cout
                << indent
                << "ReturnStatement"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            if (return_statement->argument) {
                print_ast(
                    return_statement->argument.get(),
                    depth + 1
                );
            }

            break;
        }



        case ASTNodeType::THROW_STATEMENT: {
            auto* statement = static_cast<const ThrowStatementNode*>(node);
            std::cout << indent << "ThrowStatement" << " [" << node->start << ", " << node->end << ")\n";
            print_ast(statement->argument.get(), depth + 1);
            break;
        }

        case ASTNodeType::TRY_STATEMENT: {
            auto* statement = static_cast<const TryStatementNode*>(node);
            std::cout << indent << "TryStatement" << " [" << node->start << ", " << node->end << ")\n";
            std::cout << indent << "  Try:\n";
            print_ast(statement->block.get(), depth + 2);
            if (statement->handler) {
                std::cout << indent << "  Catch(pattern):\n";
                print_ast(statement->handler.get(), depth + 2);
            }
            if (statement->finalizer) {
                std::cout << indent << "  Finally:\n";
                print_ast(statement->finalizer.get(), depth + 2);
            }
            break;
        }

        case ASTNodeType::IF_STATEMENT: {
            auto* statement = static_cast<const IfStatementNode*>(node);

            std::cout
                << indent
                << "IfStatement"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout << indent << "  Test:\n";
            print_ast(statement->test.get(), depth + 2);

            std::cout << indent << "  Consequent:\n";
            print_ast(statement->consequent.get(), depth + 2);

            if (statement->alternate) {
                std::cout << indent << "  Alternate:\n";
                print_ast(statement->alternate.get(), depth + 2);
            }

            break;
        }

        case ASTNodeType::WHILE_STATEMENT: {
            auto* statement = static_cast<const WhileStatementNode*>(node);

            std::cout
                << indent
                << "WhileStatement"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout << indent << "  Test:\n";
            print_ast(statement->test.get(), depth + 2);

            std::cout << indent << "  Body:\n";
            print_ast(statement->body.get(), depth + 2);

            break;
        }

        case ASTNodeType::DO_WHILE_STATEMENT: {
            auto* statement = static_cast<const DoWhileStatementNode*>(node);
            std::cout << indent << "DoWhileStatement" << " [" << node->start << ", " << node->end << ")\n";
            print_ast(statement->body.get(), depth + 1);
            print_ast(statement->test.get(), depth + 1);
            break;
        }
        case ASTNodeType::FOR_STATEMENT: {
            auto* statement = static_cast<const ForStatementNode*>(node);
            std::cout << indent << "ForStatement" << " [" << node->start << ", " << node->end << ")\n";
            if (statement->init) print_ast(statement->init.get(), depth + 1);
            if (statement->test) print_ast(statement->test.get(), depth + 1);
            if (statement->update) print_ast(statement->update.get(), depth + 1);
            print_ast(statement->body.get(), depth + 1);
            break;
        }
        case ASTNodeType::FOR_IN_STATEMENT: {
            auto* statement = static_cast<const ForInStatementNode*>(node);
            std::cout << indent << "ForInStatement" << " [" << node->start << ", " << node->end << ")\n";
            print_ast(statement->left.get(), depth + 1);
            print_ast(statement->object.get(), depth + 1);
            print_ast(statement->body.get(), depth + 1);
            break;
        }
        case ASTNodeType::FOR_OF_STATEMENT: {
            auto* statement = static_cast<const ForOfStatementNode*>(node);
            std::cout << indent << "ForOfStatement" << " [" << node->start << ", " << node->end << ")\n";
            print_ast(statement->left.get(), depth + 1);
            print_ast(statement->iterable.get(), depth + 1);
            print_ast(statement->body.get(), depth + 1);
            break;
        }
        case ASTNodeType::SWITCH_STATEMENT: {
            auto* statement = static_cast<const SwitchStatementNode*>(node);
            std::cout << indent << "SwitchStatement" << " [" << node->start << ", " << node->end << ")\n";
            print_ast(statement->discriminant.get(), depth + 1);
            for (const auto& clause : statement->cases) {
                if (clause.test) print_ast(clause.test.get(), depth + 1);
                for (const auto& child : clause.consequent) print_ast(child.get(), depth + 2);
            }
            break;
        }
        case ASTNodeType::LABELED_STATEMENT: {
            auto* statement = static_cast<const LabeledStatementNode*>(node);
            std::cout << indent << "LabeledStatement(" << statement->label << ") [" << node->start << ", " << node->end << ")\n";
            print_ast(statement->body.get(), depth + 1);
            break;
        }

        case ASTNodeType::BREAK_STATEMENT: {
            std::cout
                << indent
                << "BreakStatement"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";
            break;
        }

        case ASTNodeType::CONTINUE_STATEMENT: {
            std::cout << indent << "ContinueStatement" << " [" << node->start << ", " << node->end << ")\n";
            break;
        }
        case ASTNodeType::DEBUGGER_STATEMENT: {
            std::cout << indent << "DebuggerStatement" << " [" << node->start << ", " << node->end << ")\n";
            break;
        }

        // ----------------------------------------------------
        // Patterns
        // ----------------------------------------------------

        case ASTNodeType::REST_ELEMENT: {
            auto* rest = static_cast<const RestElementNode*>(node);
            std::cout << indent << "RestElement" << " [" << node->start << ", " << node->end << ")\n";
            print_ast(rest->argument.get(), depth + 1);
            break;
        }

        case ASTNodeType::ASSIGNMENT_PATTERN: {
            auto* pattern = static_cast<const AssignmentPatternNode*>(node);
            std::cout << indent << "AssignmentPattern" << " [" << node->start << ", " << node->end << ")\n";
            print_ast(pattern->left.get(), depth + 1);
            print_ast(pattern->right.get(), depth + 1);
            break;
        }

        case ASTNodeType::ARRAY_PATTERN: {
            auto* pattern = static_cast<const ArrayPatternNode*>(node);
            std::cout << indent << "ArrayPattern" << " [" << node->start << ", " << node->end << ")\n";
            for (const auto& element : pattern->elements) {
                if (element) print_ast(element.get(), depth + 1);
                else std::cout << std::string(static_cast<std::size_t>(depth + 1) * 2U, ' ') << "Elision\n";
            }
            break;
        }

        case ASTNodeType::OBJECT_PATTERN_PROPERTY: {
            auto* property = static_cast<const ObjectPatternPropertyNode*>(node);
            std::cout << indent << "ObjectPatternProperty" << (property->computed ? "(computed)" : "")
                      << " [" << node->start << ", " << node->end << ")\n";
            print_ast(property->key.get(), depth + 1);
            print_ast(property->value.get(), depth + 1);
            break;
        }

        case ASTNodeType::OBJECT_PATTERN: {
            auto* pattern = static_cast<const ObjectPatternNode*>(node);
            std::cout << indent << "ObjectPattern" << " [" << node->start << ", " << node->end << ")\n";
            for (const auto& property : pattern->properties) print_ast(property.get(), depth + 1);
            if (pattern->rest) print_ast(pattern->rest.get(), depth + 1);
            break;
        }

        // ----------------------------------------------------
        // Variables
        // ----------------------------------------------------

        case ASTNodeType::VARIABLE_DECLARATION: {
            auto* declaration =
                static_cast<const VariableDeclarationNode*>(node);

            std::cout
                << indent
                << "VariableDeclaration("
                << variable_kind_to_string(declaration->kind)
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            for (const auto& declarator : declaration->declarations) {
                print_ast(
                    declarator.get(),
                    depth + 1
                );
            }

            break;
        }


        case ASTNodeType::VARIABLE_DECLARATOR: {
            auto* declarator =
                static_cast<const VariableDeclaratorNode*>(node);

            std::cout
                << indent
                << "VariableDeclarator"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout
                << indent
                << "  Id:\n";

            print_ast(
                declarator->id.get(),
                depth + 2
            );

            if (declarator->init) {
                std::cout
                    << indent
                    << "  Init:\n";

                print_ast(
                    declarator->init.get(),
                    depth + 2
                );
            }

            break;
        }

        // ----------------------------------------------------
        // Functions
        // ----------------------------------------------------

        case ASTNodeType::FUNCTION_EXPR: {
            auto* expression = static_cast<const FunctionExpressionNode*>(node);
            std::cout << indent << "FunctionExpr("
                      << (expression->id ? std::string(expression->id->name) : std::string("<anonymous>"));
            if (expression->generator) std::cout << ", generator";
            std::cout << ") [" << node->start << ", " << node->end << ")\n";
            if (expression->id) print_ast(expression->id.get(), depth + 1);
            for (const auto& param : expression->params) print_ast(param.get(), depth + 1);
            print_ast(expression->body.get(), depth + 1);
            break;
        }

        case ASTNodeType::FUNCTION_DECLARATION: {
            auto* declaration =
                static_cast<const FunctionDeclarationNode*>(node);

            std::cout
                << indent
                << "FunctionDeclaration("
                << declaration->id->name;

            if (declaration->async) {
                std::cout << ", async";
            }

            if (declaration->generator) {
                std::cout << ", generator";
            }

            std::cout
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout
                << indent
                << "  Id:\n";

            print_ast(declaration->id.get(), depth + 2);


            std::cout
                << indent
                << "  params:\n";

            for (const auto& param : declaration->params) {
                print_ast(
                    param.get(),
                    depth + 2
                );
            }

            std::cout
                << indent
                << "  body:\n";
  
            print_ast(
                declaration->body.get(),
                depth + 2
            );
            

            break;
        }

        // ----------------------------------------------------
        // Modules
        // ----------------------------------------------------

        case ASTNodeType::IMPORT_SPECIFIER: {
            auto* specifier = static_cast<const ImportSpecifierNode*>(node);

            std::cout
                << indent
                << "ImportSpecifier"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout << indent << "  Imported:\n";
            print_ast(specifier->imported.get(), depth + 2);

            std::cout << indent << "  Local:\n";
            print_ast(specifier->local.get(), depth + 2);

            break;
        }

        case ASTNodeType::IMPORT_DECLARATION: {
            auto* declaration = static_cast<const ImportDeclarationNode*>(node);

            std::cout
                << indent
                << "ImportDeclaration"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            if (!declaration->specifiers.empty()) {
                std::cout << indent << "  Specifiers:\n";
                for (const auto& specifier : declaration->specifiers) {
                    print_ast(specifier.get(), depth + 2);
                }
            }

            std::cout << indent << "  Source:\n";
            print_ast(declaration->source.get(), depth + 2);

            break;
        }

        case ASTNodeType::EXPORT_NAMED_DECLARATION: {
            auto* declaration = static_cast<const ExportNamedDeclarationNode*>(node);

            std::cout
                << indent
                << "ExportNamedDeclaration"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            print_ast(declaration->declaration.get(), depth + 1);
            
            if (!declaration->specifiers.empty()) {
                std::cout << indent << "  Specifiers:\n";
                for (const auto& specifier : declaration->specifiers) {
                    print_ast(specifier.get(), depth + 2);
                }
            }

            break;
        }

        // ----------------------------------------------------
        // Classes
        // ----------------------------------------------------

        case ASTNodeType::METHOD_DEFINITION: {
            auto* method = static_cast<const MethodDefinitionNode*>(node);

            std::cout
                << indent
                << "MethodDefinition("
                << method->key->name;

            if (method->async) {
                std::cout << ", async";
            }

            if (method->generator) {
                std::cout << ", generator";
            }

            std::cout
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout << indent << "  Key:\n";
            print_ast(method->key.get(), depth + 2);

            std::cout << indent << "  Params:\n";
            for (const auto& param : method->params) {
                print_ast(param.get(), depth + 2);
            }

            std::cout << indent << "  Body:\n";
            print_ast(method->body.get(), depth + 2);

            break;
        }

        case ASTNodeType::CLASS_DECLARATION: {
            auto* declaration = static_cast<const ClassDeclarationNode*>(node);

            std::cout
                << indent
                << "ClassDeclaration("
                << declaration->id->name
                << ")"
                << " [" << node->start << ", " << node->end << ")"
                << "\n";

            std::cout << indent << "  Id:\n";
            print_ast(declaration->id.get(), depth + 2);

            if (declaration->super_class) {
                std::cout << indent << "  SuperClass:\n";
                print_ast(declaration->super_class.get(), depth + 2);
            }

            if (!declaration->methods.empty()) {
                std::cout << indent << "  Methods:\n";
                for (const auto& method : declaration->methods) {
                    print_ast(method.get(), depth + 2);
                }
            }

            break;
        }
    }
}



} // namespace js::frontend

#endif
