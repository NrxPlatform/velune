#include "scope.hpp"

#include <algorithm>

#include <limits>
#include <string>
#include <utility>

#include <js/error.hpp>

namespace js::compiler::detail {
namespace {

void collect_pattern_identifiers(const frontend::ASTNode& node, std::vector<const frontend::IdentifierNode*>& out) {
    switch (node.type) {
    case frontend::ASTNodeType::IDENTIFIER:
        out.push_back(static_cast<const frontend::IdentifierNode*>(&node));
        return;
    case frontend::ASTNodeType::ASSIGNMENT_PATTERN:
        collect_pattern_identifiers(*static_cast<const frontend::AssignmentPatternNode&>(node).left, out);
        return;
    case frontend::ASTNodeType::REST_ELEMENT:
        collect_pattern_identifiers(*static_cast<const frontend::RestElementNode&>(node).argument, out);
        return;
    case frontend::ASTNodeType::ARRAY_PATTERN:
        for (const auto& element : static_cast<const frontend::ArrayPatternNode&>(node).elements)
            if (element) collect_pattern_identifiers(*element, out);
        return;
    case frontend::ASTNodeType::OBJECT_PATTERN: {
        const auto& pattern = static_cast<const frontend::ObjectPatternNode&>(node);
        for (const auto& property : pattern.properties) collect_pattern_identifiers(*property->value, out);
        if (pattern.rest) collect_pattern_identifiers(*pattern.rest, out);
        return;
    }
    default:
        return;
    }
}

std::vector<const frontend::IdentifierNode*> pattern_identifiers(const frontend::ASTNode& node) {
    std::vector<const frontend::IdentifierNode*> out;
    collect_pattern_identifiers(node, out);
    return out;
}

bool simple_identifier_parameters(const std::vector<std::unique_ptr<frontend::ASTNode>>& params,
                                  const std::vector<std::unique_ptr<frontend::ASTNode>>& defaults,
                                  std::optional<std::size_t> rest) {
    if (rest) return false;
    for (const auto& value : defaults) if (value) return false;
    for (const auto& parameter : params) if (parameter->type != frontend::ASTNodeType::IDENTIFIER) return false;
    return true;
}

[[nodiscard]] const frontend::ASTNode* declaration_node(const frontend::ASTNode* node) noexcept {
    if (node != nullptr && node->type == frontend::ASTNodeType::EXPORT_NAMED_DECLARATION) {
        const auto& exported = static_cast<const frontend::ExportNamedDeclarationNode&>(*node);
        return exported.declaration.get();
    }
    return node;
}
}

Error ScopeStack::error_at(const frontend::ASTNode& node, std::string message) const {
    return Error{ErrorCode::compile_error, "scope error at [" + std::to_string(node.start) + ", " + std::to_string(node.end) + "): " + std::move(message)};
}

BindingKind ScopeStack::binding_kind(frontend::VariableKind kind) noexcept {
    switch (kind) {
    case frontend::VariableKind::VAR: return BindingKind::var_binding;
    case frontend::VariableKind::LET: return BindingKind::let_binding;
    case frontend::VariableKind::CONST: return BindingKind::const_binding;
    }
    return BindingKind::let_binding;
}

Result<Binding*> ScopeStack::declare_in_scope(Scope& scope, std::string_view name, BindingKind kind, const frontend::ASTNode& declaration, bool allow_repeated_var) {
    const auto existing = scope.bindings.find(name);
    if (existing != scope.bindings.end()) {
        const bool var_compatible = allow_repeated_var && kind == BindingKind::var_binding &&
            (existing->second.kind == BindingKind::var_binding || existing->second.kind == BindingKind::parameter_binding || existing->second.kind == BindingKind::function_binding);
        if (var_compatible) return &existing->second;
        return error_at(declaration, "duplicate or conflicting binding '" + std::string(name) + "'");
    }
    if (next_slot_ == std::numeric_limits<std::uint32_t>::max()) return error_at(declaration, "local slot space exhausted");
    const auto slot = next_slot_++;
    js::BindingState initial_state = js::BindingState::Uninitialized;
    if (kind == BindingKind::var_binding || kind == BindingKind::function_binding || kind == BindingKind::parameter_binding)
        initial_state = js::BindingState::InitializedMutable;
    initial_binding_states_.push_back(initial_state);
    binding_immutable_.push_back(kind == BindingKind::const_binding || kind == BindingKind::import_binding);
    auto [position, inserted] = scope.bindings.emplace(name, Binding{name, slot, kind, declaration.start, declaration.end});
    (void)inserted;
    return &position->second;
}

void ScopeStack::collect_var_names(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements, std::vector<std::pair<std::string_view, const frontend::ASTNode*>>& names) const {
    auto visit = [&](auto&& self, const frontend::ASTNode* raw) -> void {
        const auto* node = declaration_node(raw);
        if (node == nullptr) return;
        switch (node->type) {
        case frontend::ASTNodeType::VARIABLE_DECLARATION: {
            const auto& declaration = static_cast<const frontend::VariableDeclarationNode&>(*node);
            if (declaration.kind == frontend::VariableKind::VAR) {
                for (const auto& declarator : declaration.declarations)
                    for (const auto* identifier : pattern_identifiers(*declarator->id)) names.emplace_back(identifier->name, identifier);
            }
            break;
        }
        case frontend::ASTNodeType::BLOCK_STATEMENT:
            for (const auto& child : static_cast<const frontend::BlockStatementNode&>(*node).body) self(self, child.get());
            break;
        case frontend::ASTNodeType::TRY_STATEMENT: {
            const auto& statement = static_cast<const frontend::TryStatementNode&>(*node);
            self(self, statement.block.get());
            if (statement.handler) self(self, statement.handler.get());
            if (statement.finalizer) self(self, statement.finalizer.get());
            break;
        }
        case frontend::ASTNodeType::IF_STATEMENT: {
            const auto& statement = static_cast<const frontend::IfStatementNode&>(*node);
            self(self, statement.consequent.get());
            if (statement.alternate) self(self, statement.alternate.get());
            break;
        }
        case frontend::ASTNodeType::WHILE_STATEMENT:
            self(self, static_cast<const frontend::WhileStatementNode&>(*node).body.get());
            break;
        case frontend::ASTNodeType::DO_WHILE_STATEMENT:
            self(self, static_cast<const frontend::DoWhileStatementNode&>(*node).body.get());
            break;
        case frontend::ASTNodeType::FOR_STATEMENT: {
            const auto& loop = static_cast<const frontend::ForStatementNode&>(*node);
            if (loop.init) self(self, loop.init.get());
            self(self, loop.body.get());
            break;
        }
        case frontend::ASTNodeType::FOR_IN_STATEMENT: {
            const auto& loop = static_cast<const frontend::ForInStatementNode&>(*node);
            self(self, loop.left.get()); self(self, loop.body.get());
            break;
        }
        case frontend::ASTNodeType::FOR_OF_STATEMENT: {
            const auto& loop = static_cast<const frontend::ForOfStatementNode&>(*node);
            self(self, loop.left.get()); self(self, loop.body.get());
            break;
        }
        case frontend::ASTNodeType::SWITCH_STATEMENT: {
            const auto& statement = static_cast<const frontend::SwitchStatementNode&>(*node);
            for (const auto& clause : statement.cases)
                for (const auto& child : clause.consequent) self(self, child.get());
            break;
        }
        case frontend::ASTNodeType::LABELED_STATEMENT:
            self(self, static_cast<const frontend::LabeledStatementNode&>(*node).body.get());
            break;
        default:
            break;
        }
    };
    for (const auto& statement : statements) visit(visit, statement.get());
}

Result<void> ScopeStack::predeclare_var_bindings(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements) {
    std::vector<std::pair<std::string_view, const frontend::ASTNode*>> names;
    collect_var_names(statements, names);
    Scope& var_scope = scopes_.back();
    for (const auto& [name, node] : names) {
        const auto declared = declare_in_scope(var_scope, name, BindingKind::var_binding, *node, true);
        if (!declared) return declared.error();
    }
    return {};
}

Result<void> ScopeStack::predeclare_function_bindings(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements) {
    Scope& scope = scopes_.back();
    for (const auto& statement : statements) {
        const auto* effective = declaration_node(statement.get());
        if (effective == nullptr || effective->type != frontend::ASTNodeType::FUNCTION_DECLARATION) continue;
        const auto& function = static_cast<const frontend::FunctionDeclarationNode&>(*effective);
        const auto declared = declare_in_scope(scope, function.id->name, BindingKind::function_binding, *function.id, false);
        if (!declared) return declared.error();
    }
    return {};
}

Result<void> ScopeStack::predeclare_lexical_bindings(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements) {
    Scope& current = scopes_.back();
    for (const auto& statement : statements) {
        if (statement->type == frontend::ASTNodeType::IMPORT_DECLARATION) {
            const auto& import_decl = static_cast<const frontend::ImportDeclarationNode&>(*statement);
            for (const auto& specifier : import_decl.specifiers) {
                const auto declared = declare_in_scope(current, specifier->local->name, BindingKind::import_binding, *specifier->local, false);
                if (!declared) return declared.error();
            }
            continue;
        }
        const auto* effective = declaration_node(statement.get());
        if (effective == nullptr) continue;
        if (effective->type == frontend::ASTNodeType::CLASS_DECLARATION) {
            const auto& declaration = static_cast<const frontend::ClassDeclarationNode&>(*effective);
            const auto declared = declare_in_scope(current, declaration.id->name, BindingKind::class_binding, *declaration.id, false);
            if (!declared) return declared.error();
            continue;
        }
        if (effective->type != frontend::ASTNodeType::VARIABLE_DECLARATION) continue;
        const auto& declaration = static_cast<const frontend::VariableDeclarationNode&>(*effective);
        if (declaration.kind == frontend::VariableKind::VAR) continue;
        for (const auto& declarator : declaration.declarations) {
            const auto identifiers = pattern_identifiers(*declarator->id);
            if (identifiers.empty()) return error_at(*declarator, "variable declarator pattern has no bound names");
            for (const auto* identifier : identifiers) {
                const auto declared = declare_in_scope(current, identifier->name, binding_kind(declaration.kind), *identifier, false);
                if (!declared) return declared.error();
            }
        }
    }
    return {};
}


Result<void> ScopeStack::validate_var_lexical_conflicts(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements) const {
    const Scope& current = scopes_.back();
    std::vector<std::pair<std::string_view, const frontend::ASTNode*>> names;
    collect_var_names(statements, names);
    for (const auto& [name, node] : names) {
        const auto lexical = current.bindings.find(name);
        if (lexical != current.bindings.end() && (lexical->second.kind == BindingKind::let_binding || lexical->second.kind == BindingKind::const_binding || lexical->second.kind == BindingKind::class_binding)) {
            return error_at(*node, "var declaration conflicts with lexical binding '" + std::string(name) + "'");
        }
    }
    return {};
}

Result<void> ScopeStack::begin_program(const frontend::ProgramNode& program) {
    scopes_.clear();
    next_slot_ = 0;
    arguments_slot_.reset();
    initial_binding_states_.clear();
    binding_immutable_.clear();
    scopes_.emplace_back();
    const auto vars = predeclare_var_bindings(program.body); if (!vars) return vars.error();
    const auto functions = predeclare_function_bindings(program.body); if (!functions) return functions.error();
    const auto lexicals = predeclare_lexical_bindings(program.body); if (!lexicals) return lexicals.error();
    return validate_var_lexical_conflicts(program.body);
}

Result<void> ScopeStack::begin_function(const frontend::FunctionDeclarationNode& function, bool allow_duplicate_parameters) {
    scopes_.clear(); next_slot_ = 0; arguments_slot_.reset(); initial_binding_states_.clear(); binding_immutable_.clear(); scopes_.emplace_back();
    const auto self = declare_in_scope(scopes_.front(), function.id->name, BindingKind::function_binding, *function.id, false); if (!self) return self.error();
    const bool simple = simple_identifier_parameters(function.params, function.param_defaults, function.rest_parameter);
    for (const auto& parameter : function.params) {
        for (const auto* identifier : pattern_identifiers(*parameter)) {
            auto existing = scopes_.front().bindings.find(identifier->name);
            if (existing != scopes_.front().bindings.end() && allow_duplicate_parameters && simple) {
                if (next_slot_ == std::numeric_limits<std::uint32_t>::max()) return error_at(*identifier, "local slot space exhausted");
                const auto slot = next_slot_++; initial_binding_states_.push_back(js::BindingState::InitializedMutable); binding_immutable_.push_back(false);
                existing->second = Binding{identifier->name, slot, BindingKind::parameter_binding, identifier->start, identifier->end};
            } else {
                const auto binding = declare_in_scope(scopes_.front(), identifier->name, BindingKind::parameter_binding, *identifier, false); if (!binding) return binding.error();
            }
        }
    }
    if (scopes_.front().bindings.find("arguments") == scopes_.front().bindings.end()) {
        const auto arguments = declare_in_scope(scopes_.front(), "arguments", BindingKind::var_binding, function, false); if (!arguments) return arguments.error(); arguments_slot_ = arguments.value()->slot;
    }
    if (!simple) for (const auto& parameter : function.params) for (const auto* identifier : pattern_identifiers(*parameter)) {
        const auto found = scopes_.front().bindings.find(identifier->name); if (found != scopes_.front().bindings.end()) initial_binding_states_[found->second.slot] = js::BindingState::Uninitialized;
    }
    return {};
}

Result<void> ScopeStack::begin_function(const frontend::FunctionExpressionNode& function, bool allow_duplicate_parameters) {
    scopes_.clear(); next_slot_ = 0; arguments_slot_.reset(); initial_binding_states_.clear(); binding_immutable_.clear();

    // Slot zero is the callee/self slot expected by VM::invoke_function.  A named
    // FunctionExpression exposes that slot through an outer name environment; an
    // anonymous FunctionExpression keeps it as an implementation-only temporary.
    scopes_.emplace_back();
    if (function.id) {
        const auto self = declare_in_scope(scopes_.front(), function.id->name, BindingKind::function_binding, *function.id, false);
        if (!self) return self.error();
    } else {
        (void)allocate_temporary();
    }

    // FunctionExpression name binding is outside the parameter environment, so a
    // parameter may shadow the inner function name without leaking it outward.
    scopes_.emplace_back();
    const bool simple = simple_identifier_parameters(function.params, function.param_defaults, function.rest_parameter);
    for (const auto& parameter : function.params) {
        for (const auto* identifier : pattern_identifiers(*parameter)) {
            auto existing = scopes_.back().bindings.find(identifier->name);
            if (existing != scopes_.back().bindings.end() && allow_duplicate_parameters && simple) {
                if (next_slot_ == std::numeric_limits<std::uint32_t>::max()) return error_at(*identifier, "local slot space exhausted");
                const auto slot = next_slot_++; initial_binding_states_.push_back(js::BindingState::InitializedMutable); binding_immutable_.push_back(false);
                existing->second = Binding{identifier->name, slot, BindingKind::parameter_binding, identifier->start, identifier->end};
            } else {
                const auto binding = declare_in_scope(scopes_.back(), identifier->name, BindingKind::parameter_binding, *identifier, false);
                if (!binding) return binding.error();
            }
        }
    }
    if (scopes_.back().bindings.find("arguments") == scopes_.back().bindings.end()) {
        const auto arguments = declare_in_scope(scopes_.back(), "arguments", BindingKind::var_binding, function, false);
        if (!arguments) return arguments.error();
        arguments_slot_ = arguments.value()->slot;
    }
    if (!simple) {
        for (const auto& parameter : function.params) for (const auto* identifier : pattern_identifiers(*parameter)) {
            const auto found = scopes_.back().bindings.find(identifier->name);
            if (found != scopes_.back().bindings.end()) initial_binding_states_[found->second.slot] = js::BindingState::Uninitialized;
        }
    }
    return {};
}

Result<void> ScopeStack::begin_method(const frontend::MethodDefinitionNode& method) {
    scopes_.clear(); next_slot_ = 0; arguments_slot_.reset(); initial_binding_states_.clear(); binding_immutable_.clear(); scopes_.emplace_back();
    const auto self = declare_in_scope(scopes_.front(), method.key->name, BindingKind::function_binding, *method.key, false); if (!self) return self.error();
    const bool simple = simple_identifier_parameters(method.params, method.param_defaults, method.rest_parameter);
    for (const auto& parameter : method.params) for (const auto* identifier : pattern_identifiers(*parameter)) {
        const auto binding = declare_in_scope(scopes_.front(), identifier->name, BindingKind::parameter_binding, *identifier, false); if (!binding) return binding.error();
    }
    if (scopes_.front().bindings.find("arguments") == scopes_.front().bindings.end()) {
        const auto arguments = declare_in_scope(scopes_.front(), "arguments", BindingKind::var_binding, method, false); if (!arguments) return arguments.error(); arguments_slot_ = arguments.value()->slot;
    }
    if (!simple) for (const auto& parameter : method.params) for (const auto* identifier : pattern_identifiers(*parameter)) {
        const auto found = scopes_.front().bindings.find(identifier->name); if (found != scopes_.front().bindings.end()) initial_binding_states_[found->second.slot] = js::BindingState::Uninitialized;
    }
    return {};
}

Result<void> ScopeStack::begin_arrow(const frontend::ArrowFunctionExprNode& arrow) {
    scopes_.clear(); next_slot_ = 0; arguments_slot_.reset(); initial_binding_states_.clear(); binding_immutable_.clear(); scopes_.emplace_back(); (void)allocate_temporary();
    const bool simple = simple_identifier_parameters(arrow.params, arrow.param_defaults, arrow.rest_parameter);
    for (const auto& parameter : arrow.params) for (const auto* identifier : pattern_identifiers(*parameter)) {
        const auto binding = declare_in_scope(scopes_.front(), identifier->name, BindingKind::parameter_binding, *identifier, false); if (!binding) return binding.error();
    }
    if (!simple) for (const auto& parameter : arrow.params) for (const auto* identifier : pattern_identifiers(*parameter)) {
        const auto found = scopes_.front().bindings.find(identifier->name); if (found != scopes_.front().bindings.end()) initial_binding_states_[found->second.slot] = js::BindingState::Uninitialized;
    }
    return {};
}

Result<void> ScopeStack::begin_function_body(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements, bool separate_parameter_environment) {
    if (separate_parameter_environment) scopes_.emplace_back();
    const auto vars = predeclare_var_bindings(statements); if (!vars) return vars.error();
    const auto functions = predeclare_function_bindings(statements); if (!functions) return functions.error();
    const auto lexicals = predeclare_lexical_bindings(statements); if (!lexicals) return lexicals.error();
    return validate_var_lexical_conflicts(statements);
}

Result<void> ScopeStack::begin_block(const frontend::BlockStatementNode& block) {
    scopes_.emplace_back();
    const auto lexicals = predeclare_lexical_bindings(block.body);
    if (!lexicals) { scopes_.pop_back(); return lexicals.error(); }
    const auto conflicts = validate_var_lexical_conflicts(block.body);
    if (!conflicts) { scopes_.pop_back(); return conflicts.error(); }
    for (const auto& statement : block.body) {
        if (statement->type == frontend::ASTNodeType::FUNCTION_DECLARATION) {
            scopes_.pop_back();
            return error_at(*statement, "block-scoped function declarations are deferred until a later stage");
        }
    }
    return {};
}

Result<void> ScopeStack::begin_switch(const frontend::SwitchStatementNode& statement) {
    scopes_.emplace_back();
    for (const auto& clause : statement.cases) {
        const auto lexicals = predeclare_lexical_bindings(clause.consequent);
        if (!lexicals) { scopes_.pop_back(); return lexicals.error(); }
    }
    for (const auto& clause : statement.cases) {
        const auto conflicts = validate_var_lexical_conflicts(clause.consequent);
        if (!conflicts) { scopes_.pop_back(); return conflicts.error(); }
        for (const auto& child : clause.consequent) {
            if (child->type == frontend::ASTNodeType::FUNCTION_DECLARATION) {
                scopes_.pop_back();
                return error_at(*child, "switch-scoped function declarations are deferred until a later stage");
            }
        }
    }
    return {};
}

Result<void> ScopeStack::begin_catch(const frontend::ASTNode& parameter, const frontend::BlockStatementNode& block) {
    scopes_.emplace_back();
    for (const auto* identifier : pattern_identifiers(parameter)) {
        auto binding = declare_in_scope(scopes_.back(), identifier->name, BindingKind::let_binding, *identifier, false);
        if (!binding) { scopes_.pop_back(); return binding.error(); }
    }
    const auto lexicals = predeclare_lexical_bindings(block.body); if (!lexicals) { scopes_.pop_back(); return lexicals.error(); }
    const auto conflicts = validate_var_lexical_conflicts(block.body); if (!conflicts) { scopes_.pop_back(); return conflicts.error(); }
    for (const auto& statement : block.body) if (statement->type == frontend::ASTNodeType::FUNCTION_DECLARATION) {
        scopes_.pop_back(); return error_at(*statement, "block-scoped function declarations are deferred until a later stage");
    }
    return {};
}

void ScopeStack::end_block() noexcept { if (scopes_.size() > 1U) scopes_.pop_back(); }

Binding* ScopeStack::find_binding(std::string_view name) noexcept {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->bindings.find(name);
        if (found != scope->bindings.end()) return &found->second;
    }
    return nullptr;
}

const Binding* ScopeStack::find_binding(std::string_view name) const noexcept {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->bindings.find(name);
        if (found != scope->bindings.end()) return &found->second;
    }
    return nullptr;
}

Result<void> ScopeStack::validate_read(const Binding&, const frontend::ASTNode&) const {
    return {};
}

Result<void> ScopeStack::validate_write(const Binding&, const frontend::ASTNode&) const {
    // P4: TDZ and immutable-assignment checks are runtime binding operations.
    return {};
}

Result<Binding*> ScopeStack::resolve(std::string_view name, const frontend::ASTNode& use) {
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->bindings.find(name);
        if (found != scope->bindings.end()) return &found->second;
    }
    return error_at(use, "unresolved identifier '" + std::string(name) + "'");
}

Result<Binding*> ScopeStack::resolve_for_read(std::string_view name, const frontend::ASTNode& use) {
    const auto found = resolve(name, use); if (!found) return found.error();
    const auto valid = validate_read(**found, use); if (!valid) return valid.error();
    return *found;
}

Result<Binding*> ScopeStack::resolve_for_write(std::string_view name, const frontend::ASTNode& use) {
    const auto found = resolve(name, use); if (!found) return found.error();
    const auto valid = validate_write(**found, use); if (!valid) return valid.error();
    return *found;
}

Result<Binding*> ScopeStack::binding_for_declaration(frontend::VariableKind kind, std::string_view name, const frontend::ASTNode& declaration) {
    if (scopes_.empty()) return error_at(declaration, "internal scope stack is empty");
    if (kind == frontend::VariableKind::VAR) {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->bindings.find(name);
            if (found != scope->bindings.end() && (found->second.kind == BindingKind::var_binding || found->second.kind == BindingKind::parameter_binding || found->second.kind == BindingKind::function_binding)) return &found->second;
        }
        return error_at(declaration, "internal var binding metadata missing for '" + std::string(name) + "'");
    }
    Scope& target = scopes_.back();
    const auto found = target.bindings.find(name);
    if (found == target.bindings.end()) return error_at(declaration, "internal binding metadata missing for '" + std::string(name) + "'");
    return &found->second;
}

Result<Binding*> ScopeStack::binding_for_function_declaration(std::string_view name, const frontend::ASTNode& declaration) {
    if (scopes_.empty()) return error_at(declaration, "internal scope stack is empty");
    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
        const auto found = scope->bindings.find(name);
        if (found != scope->bindings.end() && found->second.kind == BindingKind::function_binding) return &found->second;
    }
    return error_at(declaration, "internal function binding metadata missing for '" + std::string(name) + "'");
}

Result<Binding*> ScopeStack::binding_for_class_declaration(std::string_view name, const frontend::ASTNode& declaration) {
    if (scopes_.empty()) return error_at(declaration, "internal scope stack is empty");
    auto& current = scopes_.back();
    const auto found = current.bindings.find(name);
    if (found == current.bindings.end()) return error_at(declaration, "internal class binding metadata missing for '" + std::string(name) + "'");
    return &found->second;
}

Result<void> ScopeStack::begin_for_declaration(const frontend::VariableDeclarationNode& declaration) {
    if (declaration.kind == frontend::VariableKind::VAR) return {};
    scopes_.push_back(Scope{});
    for (const auto& declarator : declaration.declarations) {
        for (const auto* identifier : pattern_identifiers(*declarator->id)) {
            const auto declared = declare_in_scope(scopes_.back(), identifier->name, binding_kind(declaration.kind), *identifier, false);
            if (!declared) { scopes_.pop_back(); return declared.error(); }
        }
    }
    return {};
}

std::uint32_t ScopeStack::allocate_temporary() {
    if (next_slot_ == std::numeric_limits<std::uint32_t>::max()) return next_slot_;
    initial_binding_states_.push_back(js::BindingState::InitializedMutable);
    binding_immutable_.push_back(false);
    return next_slot_++;
}

} // namespace js::compiler::detail
