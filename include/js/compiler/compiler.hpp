#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <utility>

#include <js/bytecode/builder.hpp>
#include <js/bytecode/chunk.hpp>
#include <js/context.hpp>
#include <js/frontend/ast.hpp>
#include <js/result.hpp>

namespace js::compiler {
namespace detail { class ScopeStack; }


struct ModuleImportBinding final {
    std::string name;
    std::uint32_t module_index;
};

struct ModuleExportRequest final {
    std::string export_name;
    std::string local_name;
    std::uint32_t module_index;
};

class Compiler final {
    friend Result<bytecode::BytecodeChunk> compile_module(
        Context&,
        const frontend::ProgramNode&,
        const std::vector<ModuleImportBinding>&,
        const std::vector<ModuleExportRequest>&);
public:
    explicit Compiler(Context& context);
    ~Compiler();

    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;
    Compiler(Compiler&&) = delete;
    Compiler& operator=(Compiler&&) = delete;

    [[nodiscard]] Result<bytecode::BytecodeChunk> compile(const frontend::ProgramNode& program);

private:
    enum class BindingStorage : std::uint8_t { local, upvalue, module };
    struct ResolvedBinding final {
        BindingStorage storage;
        std::uint32_t index;
        bool writable;
    };
    enum class ReferenceKind : std::uint8_t { environment, runtime_environment, static_property, computed_property };
    struct CompiledReference final {
        ReferenceKind kind{ReferenceKind::environment};
        std::optional<ResolvedBinding> binding;
        std::uint32_t base_slot{0};
        std::uint32_t key_slot{0};
        std::uint32_t key_constant{0};
        std::uint32_t name_constant{0};
        bool strict{false};
    };
    struct ControlContext final {
        std::size_t protected_finally_depth;
        bool iteration{false};
        std::vector<std::string> labels;
        std::vector<std::size_t> break_jumps;
        std::vector<std::size_t> continue_jumps;
    };

    explicit Compiler(Context& context, Compiler* parent);
    Compiler(Context& context, const std::unordered_map<std::string, std::uint32_t>* module_imports, std::uint32_t module_binding_count);

    [[nodiscard]] Result<void> compile_statement(const frontend::ASTNode& node, bool preserve_expression_value);
    [[nodiscard]] Result<void> compile_statement_list(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements, bool allow_final_expression_value, bool hoist_functions);
    [[nodiscard]] Result<void> compile_block(const frontend::BlockStatementNode& block);
    [[nodiscard]] Result<void> compile_expression(const frontend::ASTNode& node);
    [[nodiscard]] Result<void> compile_variable_declaration(const frontend::VariableDeclarationNode& declaration);
    [[nodiscard]] Result<void> compile_function_declaration(const frontend::FunctionDeclarationNode& function);
    [[nodiscard]] Result<Value> compile_function_value(const frontend::FunctionDeclarationNode& function);
    [[nodiscard]] Result<Value> compile_function_value(const frontend::FunctionExpressionNode& function, bool constructable = true);
    [[nodiscard]] Result<void> compile_function_expression(const frontend::FunctionExpressionNode& function);
    [[nodiscard]] Result<void> compile_arrow(const frontend::ArrowFunctionExprNode& arrow);
    [[nodiscard]] Result<void> compile_parameter_initializers(const std::vector<std::unique_ptr<frontend::ASTNode>>& params, const std::vector<std::unique_ptr<frontend::ASTNode>>& defaults, std::optional<std::size_t> rest_parameter);
    [[nodiscard]] Result<void> compile_binding_pattern(const frontend::ASTNode& pattern, std::uint32_t value_slot, frontend::VariableKind kind, bool parameter_binding = false);
    [[nodiscard]] Result<void> compile_assignment_pattern(const frontend::ASTNode& pattern, std::uint32_t value_slot);
    [[nodiscard]] Result<void> compile_pattern(const frontend::ASTNode& pattern, std::uint32_t value_slot, frontend::VariableKind kind, bool assignment, bool parameter_binding);
    [[nodiscard]] Result<void> compile_return(const frontend::ReturnStatementNode& statement);
    [[nodiscard]] Result<void> compile_throw(const frontend::ThrowStatementNode& statement);
    [[nodiscard]] Result<void> compile_try(const frontend::TryStatementNode& statement);
    [[nodiscard]] Result<void> compile_if(const frontend::IfStatementNode& statement);
    [[nodiscard]] Result<void> compile_while(const frontend::WhileStatementNode& statement, const std::vector<std::string>& labels = {});
    [[nodiscard]] Result<void> compile_do_while(const frontend::DoWhileStatementNode& statement, const std::vector<std::string>& labels = {});
    [[nodiscard]] Result<void> compile_for(const frontend::ForStatementNode& statement, const std::vector<std::string>& labels = {});
    [[nodiscard]] Result<void> compile_for_in(const frontend::ForInStatementNode& statement, const std::vector<std::string>& labels = {});
    [[nodiscard]] Result<void> compile_for_of(const frontend::ForOfStatementNode& statement, const std::vector<std::string>& labels = {});
    [[nodiscard]] Result<void> compile_switch(const frontend::SwitchStatementNode& statement, const std::vector<std::string>& labels = {});
    [[nodiscard]] Result<void> compile_labeled(const frontend::LabeledStatementNode& statement);
    [[nodiscard]] Result<void> compile_break(const frontend::BreakStatementNode& statement);
    [[nodiscard]] Result<void> compile_continue(const frontend::ContinueStatementNode& statement);
    [[nodiscard]] Result<void> compile_iteration_binding(const frontend::ASTNode& left, std::uint32_t value_slot);
    [[nodiscard]] Result<void> compile_call(const frontend::CallExprNode& call);
    [[nodiscard]] Result<void> compile_new(const frontend::NewExprNode& expression);
    [[nodiscard]] Result<void> compile_class_declaration(const frontend::ClassDeclarationNode& declaration);
    [[nodiscard]] Result<Value> compile_method_value(const frontend::MethodDefinitionNode& method, std::string_view display_name);
    [[nodiscard]] Result<void> compile_assignment(const frontend::AssignmentExprNode& assignment);
    [[nodiscard]] Result<void> compile_member(const frontend::MemberExprNode& member);
    [[nodiscard]] Result<void> compile_array(const frontend::ArrayExprNode& array);
    [[nodiscard]] Result<void> compile_object(const frontend::ObjectExprNode& object);
    [[nodiscard]] Result<void> compile_template(const frontend::TemplateLiteralNode& literal);
    [[nodiscard]] Result<void> compile_tagged_template(const frontend::TaggedTemplateExprNode& tagged);
    [[nodiscard]] Result<void> compile_optional_chain(const frontend::OptionalChainExprNode& chain, bool delete_final = false);
    [[nodiscard]] Result<void> compile_argument_array(const std::vector<std::unique_ptr<frontend::ASTNode>>& arguments);
    [[nodiscard]] Result<void> compile_binary(const frontend::BinaryExprNode& binary);
    [[nodiscard]] Result<void> compile_conditional(const frontend::ConditionalExprNode& conditional);
    [[nodiscard]] Result<void> compile_sequence(const frontend::SequenceExprNode& sequence);
    [[nodiscard]] Result<void> compile_logical(const frontend::LogicalExprNode& logical);
    [[nodiscard]] Result<void> compile_unary(const frontend::UnaryExprNode& unary);
    [[nodiscard]] Result<void> compile_update(const frontend::UpdateExprNode& update);
    [[nodiscard]] Result<void> compile_yield(const frontend::YieldExprNode& expression);
    [[nodiscard]] Result<void> emit_number_constant(const frontend::NumberLiteralNode& number);
    [[nodiscard]] Result<std::uint32_t> add_property_key(std::string_view key);

    [[nodiscard]] Result<CompiledReference> compile_reference(const frontend::ASTNode& target);
    [[nodiscard]] Result<void> emit_get_value(const CompiledReference& reference);
    [[nodiscard]] Result<void> emit_get_value_preserving_key(const CompiledReference& reference);
    [[nodiscard]] Result<void> emit_put_value(const CompiledReference& reference);
    void emit_get_this_value(const CompiledReference& reference);
    [[nodiscard]] bool is_property_reference(const CompiledReference& reference) const noexcept;
    [[nodiscard]] bool is_unresolvable_reference(const frontend::ASTNode& target) const noexcept;

    [[nodiscard]] Result<ResolvedBinding> resolve_read(std::string_view name, const frontend::ASTNode& use);
    [[nodiscard]] Result<ResolvedBinding> resolve_write(std::string_view name, const frontend::ASTNode& use);
    [[nodiscard]] std::optional<ResolvedBinding> resolve_capture(std::string_view name);
    [[nodiscard]] ResolvedBinding add_upvalue(bytecode::UpvalueSource source, std::uint32_t index, bool writable);
    void emit_get(const ResolvedBinding& binding);
    void emit_set(const ResolvedBinding& binding);

    [[nodiscard]] Error error_at(const frontend::ASTNode& node, std::string message) const;

    Context* context_;
    Compiler* parent_{nullptr};
    bytecode::BytecodeBuilder builder_;
    std::unique_ptr<detail::ScopeStack> scopes_;
    std::vector<bytecode::UpvalueDescriptor> upvalues_;
    std::vector<bool> upvalue_writable_;
    const std::unordered_map<std::string, std::uint32_t>* module_imports_{nullptr};
    std::uint32_t module_binding_count_{0};
    bool module_mode_{false};
    bool used_{false};
    bool in_function_{false};
    bool in_generator_{false};
    bool strict_{false};
    std::uint32_t next_dynamic_reference_slot_{0};
    std::size_t with_depth_{0};
    bool may_capture_with_{false};
    // AST ranges of active with bodies, innermost last. A lexical binding
    // declared inside the innermost body cannot be intercepted by that with.
    std::vector<std::pair<std::size_t, std::size_t>> with_body_ranges_;
    [[nodiscard]] bool direct_binding_preferred(std::string_view name) const noexcept;
    [[nodiscard]] Result<void> compile_with(const frontend::WithStatementNode& statement);
    [[nodiscard]] Result<std::uint32_t> dynamic_fallback(std::string_view name, const frontend::ASTNode& use);
    std::size_t protected_finally_depth_{0};
    std::vector<ControlContext> controls_;
};

[[nodiscard]] Result<bytecode::BytecodeChunk> compile_program(Context& context, const frontend::ProgramNode& program);
[[nodiscard]] Result<bytecode::BytecodeChunk> compile_module(
    Context& context,
    const frontend::ProgramNode& program,
    const std::vector<ModuleImportBinding>& imports,
    const std::vector<ModuleExportRequest>& exports);

} // namespace js::compiler
