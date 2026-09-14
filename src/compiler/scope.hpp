#pragma once

#include <cstdint>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>

#include <js/environment.hpp>
#include <js/frontend/ast.hpp>
#include <js/result.hpp>

namespace js::compiler::detail {

enum class BindingKind : std::uint8_t {
    var_binding,
    let_binding,
    const_binding,
    function_binding,
    class_binding,
    parameter_binding,
    import_binding,
};

struct Binding final {
    std::string_view name;
    std::uint32_t slot;
    BindingKind kind;
    std::size_t declaration_start;
    std::size_t declaration_end;
};

class ScopeStack final {
public:
    [[nodiscard]] Result<void> begin_program(const frontend::ProgramNode& program);
    [[nodiscard]] Result<void> begin_function(const frontend::FunctionDeclarationNode& function, bool allow_duplicate_parameters = false);
    [[nodiscard]] Result<void> begin_function(const frontend::FunctionExpressionNode& function, bool allow_duplicate_parameters = false);
    [[nodiscard]] Result<void> begin_method(const frontend::MethodDefinitionNode& method);
    [[nodiscard]] Result<void> begin_arrow(const frontend::ArrowFunctionExprNode& arrow);
    [[nodiscard]] Result<void> begin_function_body(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements, bool separate_parameter_environment);
    [[nodiscard]] Result<void> begin_block(const frontend::BlockStatementNode& block);
    [[nodiscard]] Result<void> begin_switch(const frontend::SwitchStatementNode& statement);
    [[nodiscard]] Result<void> begin_catch(const frontend::ASTNode& parameter, const frontend::BlockStatementNode& block);
    [[nodiscard]] Result<void> begin_for_declaration(const frontend::VariableDeclarationNode& declaration);
    [[nodiscard]] std::uint32_t allocate_temporary();
    void end_block() noexcept;

    [[nodiscard]] Binding* find_binding(std::string_view name) noexcept;
    [[nodiscard]] const Binding* find_binding(std::string_view name) const noexcept;
    [[nodiscard]] Result<void> validate_read(const Binding& binding, const frontend::ASTNode& use) const;
    [[nodiscard]] Result<void> validate_write(const Binding& binding, const frontend::ASTNode& use) const;
    [[nodiscard]] Result<Binding*> resolve_for_read(std::string_view name, const frontend::ASTNode& use);
    [[nodiscard]] Result<Binding*> resolve_for_write(std::string_view name, const frontend::ASTNode& use);
    [[nodiscard]] Result<Binding*> binding_for_declaration(frontend::VariableKind kind, std::string_view name, const frontend::ASTNode& declaration);
    [[nodiscard]] Result<Binding*> binding_for_function_declaration(std::string_view name, const frontend::ASTNode& declaration);
    [[nodiscard]] Result<Binding*> binding_for_class_declaration(std::string_view name, const frontend::ASTNode& declaration);
    void mark_initialized(Binding&) noexcept {}

    [[nodiscard]] std::uint32_t local_count() const noexcept { return next_slot_; }
    [[nodiscard]] const std::vector<js::BindingState>& local_binding_states() const noexcept { return initial_binding_states_; }
    [[nodiscard]] const std::vector<bool>& local_binding_immutable() const noexcept { return binding_immutable_; }
    [[nodiscard]] std::optional<std::uint32_t> arguments_slot() const noexcept { return arguments_slot_; }

private:
    struct Scope final { std::unordered_map<std::string_view, Binding> bindings; };

    [[nodiscard]] Result<void> predeclare_var_bindings(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements);
    [[nodiscard]] Result<void> predeclare_function_bindings(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements);
    [[nodiscard]] Result<void> predeclare_lexical_bindings(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements);
    [[nodiscard]] Result<void> validate_var_lexical_conflicts(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements) const;
    void collect_var_names(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements, std::vector<std::pair<std::string_view, const frontend::ASTNode*>>& names) const;

    [[nodiscard]] Result<Binding*> declare_in_scope(Scope& scope, std::string_view name, BindingKind kind, const frontend::ASTNode& declaration, bool allow_repeated_var);
    [[nodiscard]] Result<Binding*> resolve(std::string_view name, const frontend::ASTNode& use);
    [[nodiscard]] Error error_at(const frontend::ASTNode& node, std::string message) const;
    [[nodiscard]] static BindingKind binding_kind(frontend::VariableKind kind) noexcept;

    std::vector<Scope> scopes_;
    std::uint32_t next_slot_{0};
    std::optional<std::uint32_t> arguments_slot_;
    std::vector<js::BindingState> initial_binding_states_;
    std::vector<bool> binding_immutable_;
};

} // namespace js::compiler::detail
