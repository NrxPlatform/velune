#include "test.hpp"

#include <string>

#include <js/frontend/frontend.hpp>
#include <js/frontend/tokenizer.hpp>

using namespace js::frontend;

TEST_CASE("frontend parses expression precedence and preserves ranges") {
    const std::string source = "2 * 3 + 1;";
    auto result = parse_program(source);
    REQUIRE(result);
    REQUIRE(result.program().start == 0);
    REQUIRE(result.program().end == source.size());
    REQUIRE(result.program().body.size() == 1);

    const auto* statement = result.program().body[0].get();
    REQUIRE(statement->type == ASTNodeType::EXPRESSION_STATEMENT);
    REQUIRE(statement->start == 0);
    REQUIRE(statement->end == source.size());
}

TEST_CASE("frontend preserves left associativity") {
    const std::string source = "10 - 2 - 3;";
    auto result = parse_program(source);
    REQUIRE(result);

    const auto* statement = static_cast<const ExpressionStatementNode*>(
        result.program().body[0].get());
    REQUIRE(statement->expression->type == ASTNodeType::BINARY_EXPR);
    const auto* root = static_cast<const BinaryExprNode*>(statement->expression.get());
    REQUIRE(root->op == TokenKind::MINUS);
    REQUIRE(root->left->type == ASTNodeType::BINARY_EXPR);
    REQUIRE(root->right->type == ASTNodeType::NUMBER_LITERAL);
}

TEST_CASE("frontend parses declarations control flow functions calls and members") {
    const std::string source =
        "let x = 2; if (x < 4) { x = x + 1; } "
        "while (x < 8) x = x + 1; "
        "function f(a) { return a * 2; } f(obj.x);";
    auto result = parse_program(source);
    REQUIRE(result);
    REQUIRE(result.program().body.size() == 5);
    REQUIRE(result.program().body[0]->type == ASTNodeType::VARIABLE_DECLARATION);
    REQUIRE(result.program().body[1]->type == ASTNodeType::IF_STATEMENT);
    REQUIRE(result.program().body[2]->type == ASTNodeType::WHILE_STATEMENT);
    REQUIRE(result.program().body[3]->type == ASTNodeType::FUNCTION_DECLARATION);
    REQUIRE(result.program().body[4]->type == ASTNodeType::EXPRESSION_STATEMENT);
}

TEST_CASE("frontend reports malformed input without throwing through public boundary") {
    const std::string source = "let = ;";
    auto result = parse_program(source);
    REQUIRE(!result);
    REQUIRE(!result.diagnostic().message.empty());
    REQUIRE(result.diagnostic().location.offset <= source.size());
}

TEST_CASE("frontend rejects return outside function") {
    auto result = parse_program("return 1;");
    REQUIRE(!result);
}

TEST_CASE("frontend rejects break and continue outside loop") {
    auto break_result = parse_program("break;");
    REQUIRE(!break_result);
    auto continue_result = parse_program("continue;");
    REQUIRE(!continue_result);
}

TEST_CASE("frontend tokenizer tracks line and column ranges") {
    Tokenizer tokenizer("let x = 1;\n  x + 2;");
    const Token first = tokenizer.current();
    REQUIRE(first.kind == TokenKind::LET);
    REQUIRE(first.start == 0);
    REQUIRE(first.start_line == 1);
    REQUIRE(first.start_column == 0);

    while (tokenizer.current().kind != TokenKind::IDENTIFIER ||
           tokenizer.current().start_line != 2) {
        tokenizer.advance();
    }
    const Token second_line_x = tokenizer.current();
    REQUIRE(second_line_x.start_line == 2);
    REQUIRE(second_line_x.start_column == 2);
    REQUIRE(second_line_x.line_break_before);
}

TEST_CASE("frontend handles a malformed corpus deterministically") {
    const char* cases[] = {
        "(", "{", "let", "const x;", "if (", "while )",
        "function f(", "1 +", "x =", "/* unterminated", "\"unterminated"
    };
    for (const char* source : cases) {
        auto result = parse_program(source);
        REQUIRE(!result);
    }
}

TEST_CASE("frontend parses this as a dedicated expression") {
    auto result = parse_program("function f() { return this.x; }");
    REQUIRE(result);
    const auto* function = static_cast<const FunctionDeclarationNode*>(result.program().body[0].get());
    const auto* ret = static_cast<const ReturnStatementNode*>(function->body->body[0].get());
    const auto* member = static_cast<const MemberExprNode*>(ret->argument.get());
    REQUIRE(member->object->type == ASTNodeType::THIS_EXPR);
}

TEST_CASE("P10.1 tokenizer recognizes longest assignment and modern operator tokens") {
    struct ExpectedToken {
        const char* source;
        TokenKind kind;
    };

    const ExpectedToken cases[] = {
        {"?", TokenKind::QUESTION},
        {"?.", TokenKind::OPTIONAL_CHAIN},
        {"??", TokenKind::NULLISH},
        {"?" "?=", TokenKind::NULLISH_EQUAL},
        {"**", TokenKind::STAR_STAR},
        {"**=", TokenKind::STAR_STAR_EQUAL},
        {"+=", TokenKind::PLUS_EQUAL},
        {"-=", TokenKind::MINUS_EQUAL},
        {"*=", TokenKind::STAR_EQUAL},
        {"/=", TokenKind::SLASH_EQUAL},
        {"%=", TokenKind::PERCENT_EQUAL},
        {"<<=", TokenKind::SHIFT_LEFT_EQUAL},
        {">>=", TokenKind::SHIFT_RIGHT_EQUAL},
        {">>>=", TokenKind::SHIFT_RIGHT_UNSIGNED_EQUAL},
        {"&=", TokenKind::AMPERSAND_EQUAL},
        {"^=", TokenKind::CARET_EQUAL},
        {"|=", TokenKind::PIPE_EQUAL},
        {"&&=", TokenKind::AND_AND_EQUAL},
        {"||=", TokenKind::OR_OR_EQUAL},
    };

    for (const auto& item : cases) {
        Tokenizer tokenizer(item.source);
        REQUIRE(tokenizer.current().kind == item.kind);
        tokenizer.advance();
        REQUIRE(tokenizer.current().kind == TokenKind::END_OF_FILE);
    }
}

TEST_CASE("P10.1 tokenizer recognizes new keyword tokens without stealing identifier prefixes") {
    struct ExpectedToken {
        const char* source;
        TokenKind kind;
    };

    const ExpectedToken keywords[] = {
        {"do", TokenKind::DO},
        {"switch", TokenKind::SWITCH},
        {"case", TokenKind::CASE},
        {"default", TokenKind::DEFAULT},
        {"debugger", TokenKind::DEBUGGER},
        {"delete", TokenKind::DELETE},
        {"void", TokenKind::VOID},
        {"typeof", TokenKind::TYPEOF},
        {"instanceof", TokenKind::INSTANCEOF},
        {"in", TokenKind::IN},
    };

    for (const auto& item : keywords) {
        Tokenizer tokenizer(item.source);
        REQUIRE(tokenizer.current().kind == item.kind);
    }

    const char* identifiers[] = {
        "doing", "switcher", "casework", "defaulted", "debuggers",
        "deleted", "voided", "typeofValue", "instanceofValue", "inside"
    };
    for (const char* source : identifiers) {
        Tokenizer tokenizer(source);
        REQUIRE(tokenizer.current().kind == TokenKind::IDENTIFIER);
    }
}

TEST_CASE("P10.1 parser has explicit precedence layers and right associative exponentiation") {
    auto result = parse_program("a + b * c ** d ** e;");
    REQUIRE(result);

    const auto* statement = static_cast<const ExpressionStatementNode*>(
        result.program().body[0].get());
    const auto* add = static_cast<const BinaryExprNode*>(statement->expression.get());
    REQUIRE(add->op == TokenKind::PLUS);
    REQUIRE(add->right->type == ASTNodeType::BINARY_EXPR);

    const auto* multiply = static_cast<const BinaryExprNode*>(add->right.get());
    REQUIRE(multiply->op == TokenKind::STAR);
    const auto* exponent = static_cast<const BinaryExprNode*>(multiply->right.get());
    REQUIRE(exponent->op == TokenKind::STAR_STAR);
    REQUIRE(exponent->right->type == ASTNodeType::BINARY_EXPR);
    REQUIRE(static_cast<const BinaryExprNode*>(exponent->right.get())->op == TokenKind::STAR_STAR);
}

TEST_CASE("P10.1 parser prepares conditional and sequence expression AST nodes") {
    auto conditional_result = parse_program("a ? b : c;");
    REQUIRE(conditional_result);
    const auto* conditional_statement = static_cast<const ExpressionStatementNode*>(
        conditional_result.program().body[0].get());
    REQUIRE(conditional_statement->expression->type == ASTNodeType::CONDITIONAL_EXPR);

    auto sequence_result = parse_program("a, b, c;");
    REQUIRE(sequence_result);
    const auto* sequence_statement = static_cast<const ExpressionStatementNode*>(
        sequence_result.program().body[0].get());
    REQUIRE(sequence_statement->expression->type == ASTNodeType::SEQUENCE_EXPR);
    const auto* sequence = static_cast<const SequenceExprNode*>(sequence_statement->expression.get());
    REQUIRE(sequence->expressions.size() == 3);
}

TEST_CASE("P10.1 parser recognizes relational keyword operators and unary keyword operators") {
    auto relational = parse_program("x instanceof C; key in object;");
    REQUIRE(relational);
    REQUIRE(relational.program().body.size() == 2);

    const auto* first_statement = static_cast<const ExpressionStatementNode*>(relational.program().body[0].get());
    REQUIRE(first_statement->expression->type == ASTNodeType::BINARY_EXPR);
    REQUIRE(static_cast<const BinaryExprNode*>(first_statement->expression.get())->op == TokenKind::INSTANCEOF);

    const auto* second_statement = static_cast<const ExpressionStatementNode*>(relational.program().body[1].get());
    REQUIRE(second_statement->expression->type == ASTNodeType::BINARY_EXPR);
    REQUIRE(static_cast<const BinaryExprNode*>(second_statement->expression.get())->op == TokenKind::IN);

    auto unary = parse_program("typeof x; void x; delete obj.x;");
    REQUIRE(unary);
    REQUIRE(unary.program().body.size() == 3);
    for (const auto& statement_node : unary.program().body) {
        const auto* statement = static_cast<const ExpressionStatementNode*>(statement_node.get());
        REQUIRE(statement->expression->type == ASTNodeType::UNARY_EXPR);
    }
}

TEST_CASE("P10.1 parser recognizes compound assignment AST without lowering semantics early") {
    auto result = parse_program("obj.x += value; obj.y ?" "?= fallback;");
    REQUIRE(result);
    REQUIRE(result.program().body.size() == 2);

    const auto* first = static_cast<const ExpressionStatementNode*>(result.program().body[0].get());
    REQUIRE(first->expression->type == ASTNodeType::ASSIGNMENT_EXPR);
    REQUIRE(static_cast<const AssignmentExprNode*>(first->expression.get())->op == TokenKind::PLUS_EQUAL);

    const auto* second = static_cast<const ExpressionStatementNode*>(result.program().body[1].get());
    REQUIRE(second->expression->type == ASTNodeType::ASSIGNMENT_EXPR);
    REQUIRE(static_cast<const AssignmentExprNode*>(second->expression.get())->op == TokenKind::NULLISH_EQUAL);
}

TEST_CASE("P10.1 parser rejects invalid assignment and exponentiation grammar targets") {
    REQUIRE(!parse_program("(a + b) = c;"));
    REQUIRE(!parse_program("-2 ** 2;"));
    REQUIRE(parse_program("(-2) ** 2;"));
}

TEST_CASE("P10.1 keyword tokens remain valid IdentifierName after member dot") {
    auto result = parse_program("obj.delete; obj.in; obj.default;");
    REQUIRE(result);
    REQUIRE(result.program().body.size() == 3);
}

TEST_CASE("P10.4 frontend parses classic for for-in do switch labels and debugger") {
    auto parsed = js::frontend::parse_program(
        "outer: for(let i=0;i<2;i=i+1){do { if(i) break outer; } while(false);} "
        "for(let k in obj){} switch(x){case 1: break; default: debugger;}");
    REQUIRE(parsed);
}

TEST_CASE("P10.4 frontend rejects invalid labelled continue and duplicate switch default") {
    auto bad_continue = js::frontend::parse_program("label: { continue label; }");
    REQUIRE(!bad_continue);
    auto bad_default = js::frontend::parse_program("switch(x){default:; default:;}");
    REQUIRE(!bad_default);
}


TEST_CASE("P10.5 frontend parses reusable binding and assignment patterns") {
    auto parsed = js::frontend::parse_program(
        "let [a,,b=3,...rest]=xs; const {x:y=4,z,...others}=obj; "
        "[a,obj.x]=pair; ({x:y}=obj); function f([p],{q}){}; try{}catch({message}){};");
    REQUIRE(parsed);
}

TEST_CASE("P10.5 frontend enforces destructuring pattern early errors") {
    REQUIRE(!js::frontend::parse_program("let [a];"));
    REQUIRE(!js::frontend::parse_program("for(let [a];;);"));
    REQUIRE(!js::frontend::parse_program("let [a,...rest,b]=xs;"));
    REQUIRE(!js::frontend::parse_program("let {a,...rest,b}=obj;"));
    REQUIRE(js::frontend::parse_program("for(let [a] of xs){}"));
}


TEST_CASE("P10.7 frontend parses optional chains and rejects invalid chain targets") {
    auto parsed = parse_program("obj?.x; obj?.[key]; fn?.(); obj?.method?.();");
    REQUIRE(parsed);
    REQUIRE(parsed.program().body.size() == 4);

    auto assignment = parse_program("obj?.x = 1;");
    REQUIRE(!assignment);
    auto update = parse_program("obj?.x++;");
    REQUIRE(!update);
    REQUIRE(!parse_program("new obj?.x();"));
    REQUIRE(parse_program("(new obj)?.x;"));
}

TEST_CASE("P10.7 frontend enforces nullish coalescing mixing restriction") {
    REQUIRE(!parse_program("a ?? b || c;"));
    REQUIRE(!parse_program("a && b ?? c;"));
    REQUIRE(parse_program("(a ?? b) || c;"));
    REQUIRE(parse_program("a ?? (b || c);"));
    REQUIRE(parse_program("(a && b) ?? c;"));
}


TEST_CASE("P10.8 frontend reports declaration conflicts as early errors") {
    auto duplicate = parse_program("let x = 1; const x = 2;");
    REQUIRE(!duplicate);
    REQUIRE(duplicate.diagnostic().message.find("duplicate lexical declaration") != std::string::npos);

    auto var_conflict = parse_program("{ let x = 1; if (1) { var x = 2; } }");
    REQUIRE(!var_conflict);
    REQUIRE(var_conflict.diagnostic().message.find("var declaration conflicts with lexical declaration") != std::string::npos);

    auto parameter_conflict = parse_program("function f(a) { let a = 1; }");
    REQUIRE(!parameter_conflict);
    REQUIRE(parameter_conflict.diagnostic().message.find("lexical declaration conflicts with parameter") != std::string::npos);
}

TEST_CASE("P10.8 frontend validates strict binding and assignment early errors") {
    auto binding = parse_program("'use strict'; let eval = 1;");
    REQUIRE(!binding);
    REQUIRE(binding.diagnostic().message.find("not permitted in strict code") != std::string::npos);

    auto assignment = parse_program("'use strict'; arguments = 1;");
    REQUIRE(!assignment);
    REQUIRE(assignment.diagnostic().message.find("assignment to 'eval' or 'arguments'") != std::string::npos);

    auto update = parse_program("'use strict'; eval++;");
    REQUIRE(!update);
    REQUIRE(update.diagnostic().message.find("assignment to 'eval' or 'arguments'") != std::string::npos);
}

TEST_CASE("P10.8 frontend validates function parameter static semantics") {
    auto duplicate_non_simple = parse_program("function f(a, a = 1) { return a; }");
    REQUIRE(!duplicate_non_simple);
    REQUIRE(duplicate_non_simple.diagnostic().message.find("duplicate parameters") != std::string::npos);

    auto strict_duplicate = parse_program("function f(a, a) { 'use strict'; return a; }");
    REQUIRE(!strict_duplicate);
    REQUIRE(strict_duplicate.diagnostic().message.find("duplicate parameters") != std::string::npos);

    auto non_simple_strict = parse_program("function f(a = 1) { 'use strict'; return a; }");
    REQUIRE(!non_simple_strict);
    REQUIRE(non_simple_strict.diagnostic().message.find("non-simple parameter list") != std::string::npos);

    auto strict_restricted = parse_program("function f(arguments) { 'use strict'; }");
    REQUIRE(!strict_restricted);
    REQUIRE(strict_restricted.diagnostic().message.find("not permitted in strict code") != std::string::npos);

    auto sloppy_duplicate = parse_program("function f(a, a) { return a; }");
    REQUIRE(sloppy_duplicate);
}

TEST_CASE("P10.8 static-semantic diagnostics retain source location") {
    const std::string source = "let ok = 1;\n'use strict';\nlet eval = 2;";
    auto result = parse_program(source);
    // Directive is not a directive prologue after a declaration, so this remains sloppy.
    REQUIRE(result);

    const std::string strict_source = "'use strict';\nlet ok = 1;\nlet arguments = 2;";
    auto strict_result = parse_program(strict_source);
    REQUIRE(!strict_result);
    REQUIRE(strict_result.diagnostic().location.line == 3);
    REQUIRE(strict_result.diagnostic().location.column == 4);
}

TEST_CASE("Test262 bootstrap tokenizer recognizes boolean and null literals") {
    Tokenizer tokenizer("true false null");
    REQUIRE(tokenizer.current().kind == TokenKind::TRUE);
    tokenizer.advance();
    REQUIRE(tokenizer.current().kind == TokenKind::FALSE);
    tokenizer.advance();
    REQUIRE(tokenizer.current().kind == TokenKind::NULL_LITERAL);
    tokenizer.advance();
    REQUIRE(tokenizer.current().kind == TokenKind::END_OF_FILE);
}

TEST_CASE("Test262 bootstrap parser builds primitive literal nodes and preserves this") {
    auto result = parse_program("true; false; null; this;");
    REQUIRE(result);
    REQUIRE(result.program().body.size() == 4);
    REQUIRE(static_cast<const ExpressionStatementNode*>(result.program().body[0].get())->expression->type == ASTNodeType::BOOLEAN_LITERAL);
    REQUIRE(static_cast<const BooleanLiteralNode*>(static_cast<const ExpressionStatementNode*>(result.program().body[0].get())->expression.get())->value);
    REQUIRE(static_cast<const ExpressionStatementNode*>(result.program().body[1].get())->expression->type == ASTNodeType::BOOLEAN_LITERAL);
    REQUIRE(!static_cast<const BooleanLiteralNode*>(static_cast<const ExpressionStatementNode*>(result.program().body[1].get())->expression.get())->value);
    REQUIRE(static_cast<const ExpressionStatementNode*>(result.program().body[2].get())->expression->type == ASTNodeType::NULL_LITERAL);
    REQUIRE(static_cast<const ExpressionStatementNode*>(result.program().body[3].get())->expression->type == ASTNodeType::THIS_EXPR);
}

TEST_CASE("Test262 bootstrap parses anonymous and named FunctionExpression without weakening FunctionDeclaration") {
    auto result = parse_program(
        "let anonymous = function (x) { return x; };"
        "let named = function inner(y) { return y; };"
    );
    REQUIRE(result);
    REQUIRE(result.program().body.size() == 2);

    const auto* first_decl = static_cast<const VariableDeclarationNode*>(result.program().body[0].get());
    REQUIRE(first_decl->declarations[0]->init->type == ASTNodeType::FUNCTION_EXPR);
    const auto* anonymous = static_cast<const FunctionExpressionNode*>(first_decl->declarations[0]->init.get());
    REQUIRE(!anonymous->id);
    REQUIRE(anonymous->params.size() == 1);

    const auto* second_decl = static_cast<const VariableDeclarationNode*>(result.program().body[1].get());
    REQUIRE(second_decl->declarations[0]->init->type == ASTNodeType::FUNCTION_EXPR);
    const auto* named = static_cast<const FunctionExpressionNode*>(second_decl->declarations[0]->init.get());
    REQUIRE(named->id);
    REQUIRE(named->id->name == "inner");

    // FunctionDeclaration grammar continues to require a BindingIdentifier.
    REQUIRE(!parse_program("function () { return 1; }"));
}

TEST_CASE("Test262 bootstrap FunctionExpression works in assignment and property positions used by harness") {
    auto result = parse_program(
        "holder.method = function () { return this.value; };"
        "holder.other = function named(a, b) { return a === b ? true : false; };"
    );
    REQUIRE(result);
    REQUIRE(result.program().body.size() == 2);
}
