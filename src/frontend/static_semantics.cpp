#include <js/frontend/static_semantics.hpp>

#include <string_view>
#include <unordered_set>
#include <vector>

namespace js::frontend {
namespace {

class Validator {
public:
    std::optional<StaticSemanticError> validate(const ProgramNode& program) {
        const bool strict = has_use_strict(program.body);
        if (auto error = validate_statement_list(program.body, strict, true)) return error;
        return {};
    }

private:
    static bool is_restricted_name(std::string_view name) noexcept {
        return name == "eval" || name == "arguments";
    }

    static bool has_use_strict(const std::vector<std::unique_ptr<ASTNode>>& statements) {
        for (const auto& statement : statements) {
            if (statement->type != ASTNodeType::EXPRESSION_STATEMENT) break;
            const auto& expression_statement = static_cast<const ExpressionStatementNode&>(*statement);
            if (expression_statement.expression->type != ASTNodeType::STRING_LITERAL) break;
            const auto& literal = static_cast<const StringLiteralNode&>(*expression_statement.expression);
            if (literal.value == "\"use strict\"" || literal.value == "'use strict'") return true;
        }
        return false;
    }

    static bool simple_parameter_list(const std::vector<std::unique_ptr<ASTNode>>& params,
                                      const std::vector<std::unique_ptr<ASTNode>>& defaults,
                                      std::optional<std::size_t> rest) {
        if (rest) return false;
        for (const auto& value : defaults) if (value) return false;
        for (const auto& parameter : params) if (parameter->type != ASTNodeType::IDENTIFIER) return false;
        return true;
    }

    static void collect_bound_names(const ASTNode& node, std::vector<const IdentifierNode*>& out) {
        switch (node.type) {
        case ASTNodeType::IDENTIFIER:
            out.push_back(static_cast<const IdentifierNode*>(&node));
            return;
        case ASTNodeType::ASSIGNMENT_PATTERN:
            collect_bound_names(*static_cast<const AssignmentPatternNode&>(node).left, out);
            return;
        case ASTNodeType::REST_ELEMENT:
            collect_bound_names(*static_cast<const RestElementNode&>(node).argument, out);
            return;
        case ASTNodeType::ARRAY_PATTERN:
            for (const auto& element : static_cast<const ArrayPatternNode&>(node).elements)
                if (element) collect_bound_names(*element, out);
            return;
        case ASTNodeType::OBJECT_PATTERN: {
            const auto& pattern = static_cast<const ObjectPatternNode&>(node);
            for (const auto& property : pattern.properties) collect_bound_names(*property->value, out);
            if (pattern.rest) collect_bound_names(*pattern.rest, out);
            return;
        }
        default:
            return;
        }
    }

    static std::vector<const IdentifierNode*> bound_names(const ASTNode& node) {
        std::vector<const IdentifierNode*> names;
        collect_bound_names(node, names);
        return names;
    }

    static void collect_var_names(const ASTNode& node, std::vector<const IdentifierNode*>& out) {
        switch (node.type) {
        case ASTNodeType::VARIABLE_DECLARATION: {
            const auto& declaration = static_cast<const VariableDeclarationNode&>(node);
            if (declaration.kind != VariableKind::VAR) return;
            for (const auto& declarator : declaration.declarations) {
                auto names = bound_names(*declarator->id);
                out.insert(out.end(), names.begin(), names.end());
            }
            return;
        }
        case ASTNodeType::BLOCK_STATEMENT:
            for (const auto& statement : static_cast<const BlockStatementNode&>(node).body)
                collect_var_names(*statement, out);
            return;
        case ASTNodeType::IF_STATEMENT: {
            const auto& statement = static_cast<const IfStatementNode&>(node);
            collect_var_names(*statement.consequent, out);
            if (statement.alternate) collect_var_names(*statement.alternate, out);
            return;
        }
        case ASTNodeType::WITH_STATEMENT:
            collect_var_names(*static_cast<const WithStatementNode&>(node).body, out); return;
        case ASTNodeType::WHILE_STATEMENT:
            collect_var_names(*static_cast<const WhileStatementNode&>(node).body, out); return;
        case ASTNodeType::DO_WHILE_STATEMENT:
            collect_var_names(*static_cast<const DoWhileStatementNode&>(node).body, out); return;
        case ASTNodeType::FOR_STATEMENT: {
            const auto& statement = static_cast<const ForStatementNode&>(node);
            if (statement.init) collect_var_names(*statement.init, out);
            collect_var_names(*statement.body, out);
            return;
        }
        case ASTNodeType::FOR_IN_STATEMENT: {
            const auto& statement = static_cast<const ForInStatementNode&>(node);
            collect_var_names(*statement.left, out);
            collect_var_names(*statement.body, out);
            return;
        }
        case ASTNodeType::FOR_OF_STATEMENT: {
            const auto& statement = static_cast<const ForOfStatementNode&>(node);
            collect_var_names(*statement.left, out);
            collect_var_names(*statement.body, out);
            return;
        }
        case ASTNodeType::SWITCH_STATEMENT:
            for (const auto& clause : static_cast<const SwitchStatementNode&>(node).cases)
                for (const auto& statement : clause.consequent) collect_var_names(*statement, out);
            return;
        case ASTNodeType::LABELED_STATEMENT:
            collect_var_names(*static_cast<const LabeledStatementNode&>(node).body, out); return;
        case ASTNodeType::TRY_STATEMENT: {
            const auto& statement = static_cast<const TryStatementNode&>(node);
            collect_var_names(*statement.block, out);
            if (statement.handler) collect_var_names(*statement.handler, out);
            if (statement.finalizer) collect_var_names(*statement.finalizer, out);
            return;
        }
        case ASTNodeType::FUNCTION_DECLARATION:
        case ASTNodeType::FUNCTION_EXPR:
        case ASTNodeType::ARROW_FUNCTION_EXPR:
            return; // nested function boundary
        default:
            return;
        }
    }

    std::optional<StaticSemanticError> duplicate_bound_name(const ASTNode& pattern) const {
        std::unordered_set<std::string_view> seen;
        for (const auto* identifier : bound_names(pattern)) {
            if (!seen.insert(identifier->name).second)
                return error(*identifier, "duplicate binding '" + std::string(identifier->name) + "'");
        }
        return {};
    }

    std::optional<StaticSemanticError> validate_binding_pattern(const ASTNode& pattern, bool strict, bool duplicates_allowed) {
        if (!duplicates_allowed) {
            if (auto duplicate = duplicate_bound_name(pattern)) return duplicate;
        }
        if (strict) {
            for (const auto* identifier : bound_names(pattern)) {
                if (is_restricted_name(identifier->name))
                    return error(*identifier, "binding identifier '" + std::string(identifier->name) + "' is not permitted in strict code");
            }
        }
        return validate_expression(pattern, strict);
    }

    std::optional<StaticSemanticError> validate_parameters(const ASTNode& owner,
        const std::vector<std::unique_ptr<ASTNode>>& params,
        const std::vector<std::unique_ptr<ASTNode>>& defaults,
        std::optional<std::size_t> rest,
        bool strict,
        bool own_use_strict) {
        const bool simple = simple_parameter_list(params, defaults, rest);
        if (own_use_strict && !simple)
            return error(owner, "'use strict' directive is not permitted with a non-simple parameter list");

        std::unordered_set<std::string_view> seen;
        for (const auto& parameter : params) {
            for (const auto* identifier : bound_names(*parameter)) {
                if ((!simple || strict) && !seen.insert(identifier->name).second)
                    return error(*identifier, "duplicate parameters require a simple non-strict parameter list");
                if (simple) seen.insert(identifier->name);
                if (strict && is_restricted_name(identifier->name))
                    return error(*identifier, "parameter '" + std::string(identifier->name) + "' is not permitted in strict code");
            }
            if (auto nested = validate_expression(*parameter, strict)) return nested;
        }
        for (const auto& initializer : defaults)
            if (initializer) if (auto nested = validate_expression(*initializer, strict)) return nested;
        return {};
    }

    std::optional<StaticSemanticError> validate_statement_list(
        const std::vector<std::unique_ptr<ASTNode>>& statements, bool strict, bool var_scope_body) {
        std::unordered_set<std::string_view> lexical_names;
        for (const auto& statement : statements) {
            const ASTNode* candidate = statement.get();
            if (candidate->type == ASTNodeType::EXPORT_NAMED_DECLARATION) {
                const auto& exported = static_cast<const ExportNamedDeclarationNode&>(*candidate);
                if (exported.declaration) candidate = exported.declaration.get();
            }
            if (candidate->type != ASTNodeType::VARIABLE_DECLARATION) continue;
            const auto& declaration = static_cast<const VariableDeclarationNode&>(*candidate);
            if (declaration.kind == VariableKind::VAR) continue;
            for (const auto& declarator : declaration.declarations) {
                if (auto duplicate = duplicate_bound_name(*declarator->id)) return duplicate;
                for (const auto* identifier : bound_names(*declarator->id)) {
                    if (!lexical_names.insert(identifier->name).second)
                        return error(*identifier, "duplicate lexical declaration '" + std::string(identifier->name) + "'");
                }
            }
        }

        std::vector<const IdentifierNode*> var_names;
        for (const auto& statement : statements) collect_var_names(*statement, var_names);
        for (const auto* identifier : var_names) {
            if (lexical_names.contains(identifier->name))
                return error(*identifier, "var declaration conflicts with lexical declaration '" + std::string(identifier->name) + "'");
        }

        (void)var_scope_body;
        for (const auto& statement : statements)
            if (auto nested = validate_statement(*statement, strict)) return nested;
        return {};
    }

    std::optional<StaticSemanticError> validate_function(const FunctionDeclarationNode& function, bool inherited_strict) {
        const bool own_strict = has_use_strict(function.body->body);
        const bool strict = inherited_strict || own_strict;
        if (strict && is_restricted_name(function.id->name))
            return error(*function.id, "function name '" + std::string(function.id->name) + "' is not permitted in strict code");
        if (auto params = validate_parameters(function, function.params, function.param_defaults,
                                               function.rest_parameter, strict, own_strict)) return params;

        // A function body's top-level lexical declarations may not collide with parameter names.
        std::unordered_set<std::string_view> parameter_names;
        for (const auto& parameter : function.params)
            for (const auto* name : bound_names(*parameter)) parameter_names.insert(name->name);
        for (const auto& statement : function.body->body) {
            if (statement->type != ASTNodeType::VARIABLE_DECLARATION) continue;
            const auto& declaration = static_cast<const VariableDeclarationNode&>(*statement);
            if (declaration.kind == VariableKind::VAR) continue;
            for (const auto& declarator : declaration.declarations)
                for (const auto* name : bound_names(*declarator->id))
                    if (parameter_names.contains(name->name))
                        return error(*name, "lexical declaration conflicts with parameter '" + std::string(name->name) + "'");
        }
        return validate_statement_list(function.body->body, strict, true);
    }

    std::optional<StaticSemanticError> validate_function_expression(const FunctionExpressionNode& function, bool inherited_strict) {
        const bool own_strict = has_use_strict(function.body->body);
        const bool strict = inherited_strict || own_strict;
        if (strict && function.id && is_restricted_name(function.id->name))
            return error(*function.id, "function name '" + std::string(function.id->name) + "' is not permitted in strict code");
        if (auto params = validate_parameters(function, function.params, function.param_defaults,
                                               function.rest_parameter, strict, own_strict)) return params;

        std::unordered_set<std::string_view> parameter_names;
        for (const auto& parameter : function.params)
            for (const auto* name : bound_names(*parameter)) parameter_names.insert(name->name);
        for (const auto& statement : function.body->body) {
            if (statement->type != ASTNodeType::VARIABLE_DECLARATION) continue;
            const auto& declaration = static_cast<const VariableDeclarationNode&>(*statement);
            if (declaration.kind == VariableKind::VAR) continue;
            for (const auto& declarator : declaration.declarations)
                for (const auto* name : bound_names(*declarator->id))
                    if (parameter_names.contains(name->name))
                        return error(*name, "lexical declaration conflicts with parameter '" + std::string(name->name) + "'");
        }
        return validate_statement_list(function.body->body, strict, true);
    }

    std::optional<StaticSemanticError> validate_arrow(const ArrowFunctionExprNode& arrow, bool inherited_strict) {
        bool own_strict = false;
        if (!arrow.expression_body && arrow.body->type == ASTNodeType::BLOCK_STATEMENT)
            own_strict = has_use_strict(static_cast<const BlockStatementNode&>(*arrow.body).body);
        const bool strict = inherited_strict || own_strict;
        if (auto params = validate_parameters(arrow, arrow.params, arrow.param_defaults,
                                               arrow.rest_parameter, strict, own_strict)) return params;
        if (arrow.expression_body) return validate_expression(*arrow.body, strict);
        return validate_statement_list(static_cast<const BlockStatementNode&>(*arrow.body).body, strict, true);
    }

    std::optional<StaticSemanticError> validate_statement(const ASTNode& node, bool strict) {
        switch (node.type) {
        case ASTNodeType::EXPRESSION_STATEMENT:
            return validate_expression(*static_cast<const ExpressionStatementNode&>(node).expression, strict);
        case ASTNodeType::BLOCK_STATEMENT:
            return validate_statement_list(static_cast<const BlockStatementNode&>(node).body, strict, false);
        case ASTNodeType::RETURN_STATEMENT: {
            const auto& statement = static_cast<const ReturnStatementNode&>(node);
            return statement.argument ? validate_expression(*statement.argument, strict) : std::optional<StaticSemanticError>{};
        }
        case ASTNodeType::THROW_STATEMENT:
            return validate_expression(*static_cast<const ThrowStatementNode&>(node).argument, strict);
        case ASTNodeType::VARIABLE_DECLARATION: {
            const auto& declaration = static_cast<const VariableDeclarationNode&>(node);
            for (const auto& declarator : declaration.declarations) {
                if (auto binding = validate_binding_pattern(*declarator->id, strict, declaration.kind == VariableKind::VAR)) return binding;
                if (declarator->init) if (auto init = validate_expression(*declarator->init, strict)) return init;
            }
            return {};
        }
        case ASTNodeType::FUNCTION_DECLARATION:
            return validate_function(static_cast<const FunctionDeclarationNode&>(node), strict);
        case ASTNodeType::IF_STATEMENT: {
            const auto& statement = static_cast<const IfStatementNode&>(node);
            if (auto test = validate_expression(*statement.test, strict)) return test;
            if (auto consequent = validate_statement(*statement.consequent, strict)) return consequent;
            return statement.alternate ? validate_statement(*statement.alternate, strict) : std::optional<StaticSemanticError>{};
        }
        case ASTNodeType::WITH_STATEMENT: {
            const auto& statement = static_cast<const WithStatementNode&>(node);
            if (strict) return error(node, "with statement is forbidden in strict mode");
            if (auto object = validate_expression(*statement.object, strict)) return object;
            return validate_statement(*statement.body, strict);
        }
        case ASTNodeType::WHILE_STATEMENT: {
            const auto& statement = static_cast<const WhileStatementNode&>(node);
            if (auto test = validate_expression(*statement.test, strict)) return test;
            return validate_statement(*statement.body, strict);
        }
        case ASTNodeType::DO_WHILE_STATEMENT: {
            const auto& statement = static_cast<const DoWhileStatementNode&>(node);
            if (auto body = validate_statement(*statement.body, strict)) return body;
            return validate_expression(*statement.test, strict);
        }
        case ASTNodeType::FOR_STATEMENT: {
            const auto& statement = static_cast<const ForStatementNode&>(node);
            if (statement.init) {
                auto result = statement.init->type == ASTNodeType::VARIABLE_DECLARATION
                    ? validate_statement(*statement.init, strict) : validate_expression(*statement.init, strict);
                if (result) return result;
            }
            // ForStatement early error: a lexical binding in the head must not
            // occur among the VarDeclaredNames of the loop body.  A nested
            // function is a var-scope boundary; collect_var_names observes it.
            if (statement.init && statement.init->type == ASTNodeType::VARIABLE_DECLARATION) {
                const auto& head = static_cast<const VariableDeclarationNode&>(*statement.init);
                if (head.kind != VariableKind::VAR) {
                    std::unordered_set<std::string_view> head_names;
                    for (const auto& declarator : head.declarations)
                        for (const auto* identifier : bound_names(*declarator->id))
                            head_names.insert(identifier->name);
                    std::vector<const IdentifierNode*> body_var_names;
                    collect_var_names(*statement.body, body_var_names);
                    for (const auto* identifier : body_var_names)
                        if (head_names.contains(identifier->name))
                            return error(*identifier, "for-loop body var declaration conflicts with lexical head binding '" +
                                std::string(identifier->name) + "'");
                }
            }
            if (statement.test) if (auto test = validate_expression(*statement.test, strict)) return test;
            if (statement.update) if (auto update = validate_expression(*statement.update, strict)) return update;
            return validate_statement(*statement.body, strict);
        }
        case ASTNodeType::FOR_IN_STATEMENT: {
            const auto& statement = static_cast<const ForInStatementNode&>(node);
            auto left = statement.left->type == ASTNodeType::VARIABLE_DECLARATION
                ? validate_statement(*statement.left, strict) : validate_expression(*statement.left, strict);
            if (left) return left;
            if (auto object = validate_expression(*statement.object, strict)) return object;
            return validate_statement(*statement.body, strict);
        }
        case ASTNodeType::FOR_OF_STATEMENT: {
            const auto& statement = static_cast<const ForOfStatementNode&>(node);
            auto left = statement.left->type == ASTNodeType::VARIABLE_DECLARATION
                ? validate_statement(*statement.left, strict) : validate_expression(*statement.left, strict);
            if (left) return left;
            if (auto iterable = validate_expression(*statement.iterable, strict)) return iterable;
            return validate_statement(*statement.body, strict);
        }
        case ASTNodeType::SWITCH_STATEMENT: {
            const auto& statement = static_cast<const SwitchStatementNode&>(node);
            if (auto discriminant = validate_expression(*statement.discriminant, strict)) return discriminant;
            std::vector<std::unique_ptr<ASTNode>> const* unused = nullptr; (void)unused;
            for (const auto& clause : statement.cases) {
                if (clause.test) if (auto test = validate_expression(*clause.test, strict)) return test;
                if (auto body = validate_statement_list(clause.consequent, strict, false)) return body;
            }
            return {};
        }
        case ASTNodeType::LABELED_STATEMENT:
            return validate_statement(*static_cast<const LabeledStatementNode&>(node).body, strict);
        case ASTNodeType::TRY_STATEMENT: {
            const auto& statement = static_cast<const TryStatementNode&>(node);
            if (auto block = validate_statement(*statement.block, strict)) return block;
            if (statement.handler_param) {
                if (auto binding = validate_binding_pattern(*statement.handler_param, strict, false)) return binding;
            }
            if (statement.handler) if (auto handler = validate_statement(*statement.handler, strict)) return handler;
            return statement.finalizer ? validate_statement(*statement.finalizer, strict) : std::optional<StaticSemanticError>{};
        }
        case ASTNodeType::EXPORT_NAMED_DECLARATION: {
            const auto& exported = static_cast<const ExportNamedDeclarationNode&>(node);
            return exported.declaration ? validate_statement(*exported.declaration, strict) : std::optional<StaticSemanticError>{};
        }
        default:
            return {};
        }
    }

    std::optional<StaticSemanticError> validate_expression(const ASTNode& node, bool strict) {
        switch (node.type) {
        case ASTNodeType::IDENTIFIER: {
            const auto& identifier = static_cast<const IdentifierNode&>(node);
            if (strict && identifier.name == "yield")
                return error(identifier, "'yield' is not permitted as an IdentifierReference in strict code");
            return {};
        }
        case ASTNodeType::NUMBER_LITERAL:
        case ASTNodeType::STRING_LITERAL:
        case ASTNodeType::BOOLEAN_LITERAL:
        case ASTNodeType::NULL_LITERAL:
        case ASTNodeType::THIS_EXPR:
        case ASTNodeType::REGEXP_LITERAL:
        case ASTNodeType::TEMPLATE_ELEMENT:
            return {};
        case ASTNodeType::UNARY_EXPR: {
            const auto& expression = static_cast<const UnaryExprNode&>(node);
            if (strict && expression.op == TokenKind::DELETE && expression.argument->type == ASTNodeType::IDENTIFIER)
                return error(node, "delete of an unqualified identifier is not permitted in strict code");
            return validate_expression(*expression.argument, strict);
        }
        case ASTNodeType::UPDATE_EXPR: {
            const auto& expression = static_cast<const UpdateExprNode&>(node);
            if (strict && expression.argument->type == ASTNodeType::IDENTIFIER &&
                is_restricted_name(static_cast<const IdentifierNode&>(*expression.argument).name))
                return error(*expression.argument, "assignment to 'eval' or 'arguments' is not permitted in strict code");
            return validate_expression(*expression.argument, strict);
        }
        case ASTNodeType::BINARY_EXPR: {
            const auto& expression = static_cast<const BinaryExprNode&>(node);
            if (auto left = validate_expression(*expression.left, strict)) return left;
            return validate_expression(*expression.right, strict);
        }
        case ASTNodeType::LOGICAL_EXPR: {
            const auto& expression = static_cast<const LogicalExprNode&>(node);
            if (auto left = validate_expression(*expression.left, strict)) return left;
            return validate_expression(*expression.right, strict);
        }
        case ASTNodeType::CONDITIONAL_EXPR: {
            const auto& expression = static_cast<const ConditionalExprNode&>(node);
            if (auto test = validate_expression(*expression.test, strict)) return test;
            if (auto consequent = validate_expression(*expression.consequent, strict)) return consequent;
            return validate_expression(*expression.alternate, strict);
        }
        case ASTNodeType::SEQUENCE_EXPR:
            for (const auto& item : static_cast<const SequenceExprNode&>(node).expressions)
                if (auto nested = validate_expression(*item, strict)) return nested;
            return {};
        case ASTNodeType::ASSIGNMENT_EXPR: {
            const auto& expression = static_cast<const AssignmentExprNode&>(node);
            if (strict && expression.left->type == ASTNodeType::IDENTIFIER &&
                is_restricted_name(static_cast<const IdentifierNode&>(*expression.left).name))
                return error(*expression.left, "assignment to 'eval' or 'arguments' is not permitted in strict code");
            if (auto left = validate_expression(*expression.left, strict)) return left;
            return validate_expression(*expression.right, strict);
        }
        case ASTNodeType::MEMBER_EXPR: {
            const auto& expression = static_cast<const MemberExprNode&>(node);
            if (auto object = validate_expression(*expression.object, strict)) return object;
            return validate_expression(*expression.property, strict);
        }
        case ASTNodeType::CALL_EXPR: {
            const auto& expression = static_cast<const CallExprNode&>(node);
            if (auto callee = validate_expression(*expression.callee, strict)) return callee;
            for (const auto& argument : expression.arguments)
                if (auto nested = validate_expression(*argument, strict)) return nested;
            return {};
        }
        case ASTNodeType::NEW_EXPR: {
            const auto& expression = static_cast<const NewExprNode&>(node);
            if (auto callee = validate_expression(*expression.callee, strict)) return callee;
            for (const auto& argument : expression.arguments)
                if (auto nested = validate_expression(*argument, strict)) return nested;
            return {};
        }
        case ASTNodeType::SPREAD_ELEMENT:
            return validate_expression(*static_cast<const SpreadElementNode&>(node).argument, strict);
        case ASTNodeType::ARRAY_EXPR:
            for (const auto& element : static_cast<const ArrayExprNode&>(node).elements)
                if (element) if (auto nested = validate_expression(*element, strict)) return nested;
            return {};
        case ASTNodeType::OBJECT_EXPR:
            for (const auto& property : static_cast<const ObjectExprNode&>(node).properties)
                if (auto nested = validate_expression(*property, strict)) return nested;
            return {};
        case ASTNodeType::PROPERTY: {
            const auto& property = static_cast<const PropertyNode&>(node);
            if (property.computed) if (auto key = validate_expression(*property.key, strict)) return key;
            return validate_expression(*property.value, strict);
        }
        case ASTNodeType::FUNCTION_EXPR:
            return validate_function_expression(static_cast<const FunctionExpressionNode&>(node), strict);
        case ASTNodeType::ARROW_FUNCTION_EXPR:
            return validate_arrow(static_cast<const ArrowFunctionExprNode&>(node), strict);
        case ASTNodeType::AWAIT_EXPR:
            return validate_expression(*static_cast<const AwaitExprNode&>(node).argument, strict);
        case ASTNodeType::YIELD_EXPR: {
            const auto& expression = static_cast<const YieldExprNode&>(node);
            return expression.argument ? validate_expression(*expression.argument, strict) : std::optional<StaticSemanticError>{};
        }
        case ASTNodeType::TEMPLATE_LITERAL: {
            const auto& expression = static_cast<const TemplateLiteralNode&>(node);
            for (const auto& item : expression.expressions)
                if (auto nested = validate_expression(*item, strict)) return nested;
            return {};
        }
        case ASTNodeType::TAGGED_TEMPLATE_EXPR: {
            const auto& expression = static_cast<const TaggedTemplateExprNode&>(node);
            if (auto tag = validate_expression(*expression.tag, strict)) return tag;
            return validate_expression(*expression.quasi, strict);
        }
        case ASTNodeType::OPTIONAL_CHAIN_EXPR: {
            const auto& expression = static_cast<const OptionalChainExprNode&>(node);
            if (auto base = validate_expression(*expression.base, strict)) return base;
            for (const auto& segment : expression.segments) {
                if (segment.property) if (auto property = validate_expression(*segment.property, strict)) return property;
                for (const auto& argument : segment.arguments)
                    if (auto nested = validate_expression(*argument, strict)) return nested;
            }
            return {};
        }
        case ASTNodeType::ASSIGNMENT_PATTERN: {
            const auto& pattern = static_cast<const AssignmentPatternNode&>(node);
            if (auto left = validate_expression(*pattern.left, strict)) return left;
            return validate_expression(*pattern.right, strict);
        }
        case ASTNodeType::REST_ELEMENT:
            return validate_expression(*static_cast<const RestElementNode&>(node).argument, strict);
        case ASTNodeType::ARRAY_PATTERN:
            for (const auto& element : static_cast<const ArrayPatternNode&>(node).elements)
                if (element) if (auto nested = validate_expression(*element, strict)) return nested;
            return {};
        case ASTNodeType::OBJECT_PATTERN: {
            const auto& pattern = static_cast<const ObjectPatternNode&>(node);
            for (const auto& property : pattern.properties)
                if (auto nested = validate_expression(*property, strict)) return nested;
            return pattern.rest ? validate_expression(*pattern.rest, strict) : std::optional<StaticSemanticError>{};
        }
        case ASTNodeType::OBJECT_PATTERN_PROPERTY: {
            const auto& property = static_cast<const ObjectPatternPropertyNode&>(node);
            if (property.computed) if (auto key = validate_expression(*property.key, strict)) return key;
            return validate_expression(*property.value, strict);
        }
        default:
            return {};
        }
    }

    static StaticSemanticError error(const ASTNode& node, std::string message) {
        return StaticSemanticError{std::move(message), node.start};
    }
};

} // namespace

std::optional<StaticSemanticError> validate_static_semantics(const ProgramNode& program) {
    return Validator{}.validate(program);
}

} // namespace js::frontend
