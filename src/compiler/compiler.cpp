#include <js/compiler/compiler.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

#include "scope.hpp"
#include <js/bytecode/opcode.hpp>
#include <js/error.hpp>
#include <js/frontend/token.hpp>

namespace js::compiler {
namespace {
[[nodiscard]] const frontend::IdentifierNode* as_identifier(const frontend::ASTNode* node) noexcept {
    if (node == nullptr || node->type != frontend::ASTNodeType::IDENTIFIER) return nullptr;
    return static_cast<const frontend::IdentifierNode*>(node);
}

[[nodiscard]] std::optional<std::string_view> static_property_name(const frontend::MemberExprNode& member) noexcept {
    if (!member.computed) {
        const auto* identifier = as_identifier(member.property.get());
        if (identifier != nullptr) return identifier->name;
    }
    return std::nullopt;
}

void collect_bound_identifiers(const frontend::ASTNode& node, std::vector<const frontend::IdentifierNode*>& out) {
    switch (node.type) {
    case frontend::ASTNodeType::IDENTIFIER:
        out.push_back(static_cast<const frontend::IdentifierNode*>(&node)); return;
    case frontend::ASTNodeType::ASSIGNMENT_PATTERN:
        collect_bound_identifiers(*static_cast<const frontend::AssignmentPatternNode&>(node).left, out); return;
    case frontend::ASTNodeType::REST_ELEMENT:
        collect_bound_identifiers(*static_cast<const frontend::RestElementNode&>(node).argument, out); return;
    case frontend::ASTNodeType::ARRAY_PATTERN:
        for (const auto& element : static_cast<const frontend::ArrayPatternNode&>(node).elements) if (element) collect_bound_identifiers(*element, out);
        return;
    case frontend::ASTNodeType::OBJECT_PATTERN: {
        const auto& pattern = static_cast<const frontend::ObjectPatternNode&>(node);
        for (const auto& property : pattern.properties) collect_bound_identifiers(*property->value, out);
        if (pattern.rest) collect_bound_identifiers(*pattern.rest, out);
        return;
    }
    default: return;
    }
}

[[nodiscard]] bool simple_parameter_list(const std::vector<std::unique_ptr<frontend::ASTNode>>& params,
                                         const std::vector<std::unique_ptr<frontend::ASTNode>>& defaults,
                                         std::optional<std::size_t> rest) noexcept {
    if (rest) return false;
    for (const auto& value : defaults) if (value) return false;
    for (const auto& parameter : params) if (parameter->type != frontend::ASTNodeType::IDENTIFIER) return false;
    return true;
}

[[nodiscard]] std::uint32_t function_length(const std::vector<std::unique_ptr<frontend::ASTNode>>& defaults, std::optional<std::size_t> rest, std::size_t parameter_count) noexcept {
    std::size_t length = parameter_count;
    if (rest) length = std::min(length, *rest);
    for (std::size_t i = 0; i < defaults.size(); ++i) if (defaults[i]) { length = std::min(length, i); break; }
    return static_cast<std::uint32_t>(length);
}

[[nodiscard]] bool has_duplicate_parameter_names(const std::vector<std::unique_ptr<frontend::ASTNode>>& params) noexcept {
    std::vector<std::string_view> names;
    for (const auto& parameter : params) {
        std::vector<const frontend::IdentifierNode*> identifiers;
        collect_bound_identifiers(*parameter, identifiers);
        for (const auto* identifier : identifiers) {
            if (std::find(names.begin(), names.end(), identifier->name) != names.end()) return true;
            names.push_back(identifier->name);
        }
    }
    return false;
}

[[nodiscard]] bool has_strict_restricted_parameter(const std::vector<std::unique_ptr<frontend::ASTNode>>& params) noexcept {
    for (const auto& parameter : params) {
        std::vector<const frontend::IdentifierNode*> identifiers;
        collect_bound_identifiers(*parameter, identifiers);
        for (const auto* identifier : identifiers) if (identifier->name == "eval" || identifier->name == "arguments") return true;
    }
    return false;
}

[[nodiscard]] std::vector<std::string> parameter_names(const std::vector<std::unique_ptr<frontend::ASTNode>>& params) {
    std::vector<std::string> names;
    names.reserve(params.size());
    for (const auto& parameter : params) {
        if (parameter->type == frontend::ASTNodeType::IDENTIFIER) names.emplace_back(static_cast<const frontend::IdentifierNode&>(*parameter).name);
        else names.emplace_back("");
    }
    return names;
}

[[nodiscard]] std::optional<bytecode::OpCode> compound_assignment_opcode(frontend::TokenKind op) noexcept {
    switch (op) {
    case frontend::TokenKind::PLUS_EQUAL: return bytecode::OpCode::add;
    case frontend::TokenKind::MINUS_EQUAL: return bytecode::OpCode::subtract;
    case frontend::TokenKind::STAR_EQUAL: return bytecode::OpCode::multiply;
    case frontend::TokenKind::SLASH_EQUAL: return bytecode::OpCode::divide;
    case frontend::TokenKind::PERCENT_EQUAL: return bytecode::OpCode::remainder;
    case frontend::TokenKind::STAR_STAR_EQUAL: return bytecode::OpCode::exponentiate;
    case frontend::TokenKind::SHIFT_LEFT_EQUAL: return bytecode::OpCode::shift_left;
    case frontend::TokenKind::SHIFT_RIGHT_EQUAL: return bytecode::OpCode::shift_right;
    case frontend::TokenKind::SHIFT_RIGHT_UNSIGNED_EQUAL: return bytecode::OpCode::shift_right_unsigned;
    case frontend::TokenKind::AMPERSAND_EQUAL: return bytecode::OpCode::bitwise_and;
    case frontend::TokenKind::CARET_EQUAL: return bytecode::OpCode::bitwise_xor;
    case frontend::TokenKind::PIPE_EQUAL: return bytecode::OpCode::bitwise_or;
    default: return std::nullopt;
    }
}

[[nodiscard]] bool is_logical_assignment(frontend::TokenKind op) noexcept {
    return op == frontend::TokenKind::AND_AND_EQUAL ||
           op == frontend::TokenKind::OR_OR_EQUAL ||
           op == frontend::TokenKind::NULLISH_EQUAL;
}

[[nodiscard]] bool has_use_strict_directive(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements) noexcept {
    for (const auto& statement : statements) {
        if (statement->type != frontend::ASTNodeType::EXPRESSION_STATEMENT) return false;
        const auto& expression_statement = static_cast<const frontend::ExpressionStatementNode&>(*statement);
        if (expression_statement.expression->type != frontend::ASTNodeType::STRING_LITERAL) return false;
        const auto& literal = static_cast<const frontend::StringLiteralNode&>(*expression_statement.expression);
        if (literal.value == "\"use strict\"" || literal.value == "'use strict'") return true;
    }
    return false;
}
}

Compiler::Compiler(Context& context) : Compiler(context, nullptr) {}
Compiler::Compiler(Context& context, Compiler* parent)
    : context_(&context), parent_(parent), scopes_(std::make_unique<detail::ScopeStack>()) {
    if (parent_ != nullptr) {
        strict_ = parent_->strict_;
        module_imports_ = parent_->module_imports_;
        module_binding_count_ = parent_->module_binding_count_;
        module_mode_ = parent_->module_mode_;
    }
}
Compiler::Compiler(Context& context, const std::unordered_map<std::string, std::uint32_t>* module_imports, std::uint32_t module_binding_count)
    : context_(&context), scopes_(std::make_unique<detail::ScopeStack>()), module_imports_(module_imports), module_binding_count_(module_binding_count), module_mode_(true), strict_(true) {}
Compiler::~Compiler() = default;

Error Compiler::error_at(const frontend::ASTNode& node, std::string message) const {
    return Error{ErrorCode::compile_error, "compile error at [" + std::to_string(node.start) + ", " + std::to_string(node.end) + "): " + std::move(message)};
}


Compiler::ResolvedBinding Compiler::add_upvalue(bytecode::UpvalueSource source, std::uint32_t index, bool writable) {
    for (std::size_t i = 0; i < upvalues_.size(); ++i) {
        if (upvalues_[i].source == source && upvalues_[i].index == index) {
            return ResolvedBinding{BindingStorage::upvalue, static_cast<std::uint32_t>(i), upvalue_writable_[i]};
        }
    }
    const auto upvalue_index = static_cast<std::uint32_t>(upvalues_.size());
    upvalues_.push_back(bytecode::UpvalueDescriptor{source, index});
    upvalue_writable_.push_back(writable);
    return ResolvedBinding{BindingStorage::upvalue, upvalue_index, writable};
}

std::optional<Compiler::ResolvedBinding> Compiler::resolve_capture(std::string_view name) {
    if (auto* binding = scopes_->find_binding(name)) {
        if (binding->kind == detail::BindingKind::import_binding && module_imports_ != nullptr) {
            const auto found = module_imports_->find(std::string(name));
            if (found != module_imports_->end()) return ResolvedBinding{BindingStorage::module, found->second, false};
        }
        const bool writable = binding->kind != detail::BindingKind::const_binding && binding->kind != detail::BindingKind::function_binding && binding->kind != detail::BindingKind::class_binding && binding->kind != detail::BindingKind::import_binding;
        return ResolvedBinding{BindingStorage::local, binding->slot, writable};
    }
    if (parent_ == nullptr) return std::nullopt;
    const auto outer = parent_->resolve_capture(name);
    if (!outer) return std::nullopt;
    const auto source = outer->storage == BindingStorage::local ? bytecode::UpvalueSource::local : bytecode::UpvalueSource::upvalue;
    return add_upvalue(source, outer->index, outer->writable);
}

Result<Compiler::ResolvedBinding> Compiler::resolve_read(std::string_view name, const frontend::ASTNode& use) {
    if (auto* binding = scopes_->find_binding(name)) {
        const auto valid = scopes_->validate_read(*binding, use);
        if (!valid) return valid.error();
        if (binding->kind == detail::BindingKind::import_binding && module_imports_ != nullptr) {
            const auto found = module_imports_->find(std::string(name));
            if (found == module_imports_->end()) return error_at(use, "module import metadata missing for '" + std::string(name) + "'");
            return ResolvedBinding{BindingStorage::module, found->second, false};
        }
        const bool writable = binding->kind != detail::BindingKind::const_binding && binding->kind != detail::BindingKind::function_binding && binding->kind != detail::BindingKind::class_binding && binding->kind != detail::BindingKind::import_binding;
        return ResolvedBinding{BindingStorage::local, binding->slot, writable};
    }
    if (parent_ != nullptr) {
        const auto parent_binding = parent_->resolve_capture(name);
        if (parent_binding) {
            if (parent_binding->storage == BindingStorage::module) return *parent_binding;
            const auto source = parent_binding->storage == BindingStorage::local ? bytecode::UpvalueSource::local : bytecode::UpvalueSource::upvalue;
            return add_upvalue(source, parent_binding->index, parent_binding->writable);
        }
    }
    return error_at(use, "unresolved identifier '" + std::string(name) + "'");
}

Result<Compiler::ResolvedBinding> Compiler::resolve_write(std::string_view name, const frontend::ASTNode& use) {
    if (auto* binding = scopes_->find_binding(name)) {
        const auto valid = scopes_->validate_write(*binding, use);
        if (!valid) return valid.error();
        if (binding->kind == detail::BindingKind::import_binding && module_imports_ != nullptr) {
            const auto found = module_imports_->find(std::string(name));
            if (found == module_imports_->end()) return error_at(use, "module import metadata missing for '" + std::string(name) + "'");
            return ResolvedBinding{BindingStorage::module, found->second, false};
        }
        const bool writable = binding->kind != detail::BindingKind::const_binding;
        return ResolvedBinding{BindingStorage::local, binding->slot, writable};
    }
    if (parent_ != nullptr) {
        const auto parent_binding = parent_->resolve_capture(name);
        if (parent_binding) {
            if (parent_binding->storage == BindingStorage::module) return *parent_binding;
            const auto source = parent_binding->storage == BindingStorage::local ? bytecode::UpvalueSource::local : bytecode::UpvalueSource::upvalue;
            return add_upvalue(source, parent_binding->index, true);
        }
    }
    return error_at(use, "unresolved identifier '" + std::string(name) + "'");
}

void Compiler::emit_get(const ResolvedBinding& binding) {
    if (binding.storage == BindingStorage::local) builder_.emit_local(bytecode::OpCode::get_local, binding.index);
    else if (binding.storage == BindingStorage::upvalue) builder_.emit_upvalue(bytecode::OpCode::get_upvalue, binding.index);
    else builder_.emit_module(bytecode::OpCode::get_module, binding.index);
}

void Compiler::emit_set(const ResolvedBinding& binding) {
    if (binding.storage == BindingStorage::local) builder_.emit_local(bytecode::OpCode::set_local, binding.index);
    else if (binding.storage == BindingStorage::upvalue) builder_.emit_upvalue(bytecode::OpCode::set_upvalue, binding.index);
    else builder_.emit_module(bytecode::OpCode::set_module, binding.index);
}

Result<std::uint32_t> Compiler::add_property_key(std::string_view key) {
    return builder_.add_constant(context_->string(key));
}

Result<Compiler::CompiledReference> Compiler::compile_reference(const frontend::ASTNode& target) {
    if (const auto* identifier = as_identifier(&target)) {
        CompiledReference reference;
        reference.strict = strict_;
        if (is_unresolvable_reference(*identifier)) {
            const auto name = add_property_key(identifier->name);
            if (!name) return name.error();
            reference.kind = ReferenceKind::runtime_environment;
            reference.name_constant = *name;
            return reference;
        }
        const auto binding = resolve_write(identifier->name, *identifier);
        if (!binding) return binding.error();
        reference.kind = ReferenceKind::environment;
        reference.binding = *binding;
        return reference;
    }

    if (target.type != frontend::ASTNodeType::MEMBER_EXPR)
        return error_at(target, "expression is not a valid reference target");

    const auto& member = static_cast<const frontend::MemberExprNode&>(target);
    CompiledReference reference;
    reference.strict = strict_;
    reference.base_slot = scopes_->allocate_temporary();

    const auto base = compile_expression(*member.object);
    if (!base) return base.error();
    builder_.emit_local(bytecode::OpCode::set_local, reference.base_slot);
    builder_.emit(bytecode::OpCode::pop);

    if (member.computed) {
        reference.kind = ReferenceKind::computed_property;
        reference.key_slot = scopes_->allocate_temporary();
        const auto key = compile_expression(*member.property);
        if (!key) return key.error();
        builder_.emit_local(bytecode::OpCode::set_local, reference.key_slot);
        builder_.emit(bytecode::OpCode::pop);
        return reference;
    }

    const auto name = static_property_name(member);
    if (!name) return error_at(member, "invalid static property reference");
    const auto key = add_property_key(*name);
    if (!key) return key.error();
    reference.kind = ReferenceKind::static_property;
    reference.key_constant = *key;
    return reference;
}

Result<void> Compiler::emit_get_value(const CompiledReference& reference) {
    switch (reference.kind) {
    case ReferenceKind::environment:
        if (!reference.binding) return Error{ErrorCode::internal, "environment reference is missing binding metadata"};
        emit_get(*reference.binding);
        return {};
    case ReferenceKind::runtime_environment:
        builder_.emit_name(bytecode::OpCode::get_name, reference.name_constant);
        return {};
    case ReferenceKind::static_property:
        builder_.emit_local(bytecode::OpCode::get_local, reference.base_slot);
        builder_.emit_property(bytecode::OpCode::get_property, reference.key_constant);
        return {};
    case ReferenceKind::computed_property:
        builder_.emit_local(bytecode::OpCode::get_local, reference.base_slot);
        builder_.emit_local(bytecode::OpCode::get_local, reference.key_slot);
        builder_.emit(bytecode::OpCode::get_element);
        return {};
    }
    return Error{ErrorCode::internal, "unknown compiled reference kind"};
}

Result<void> Compiler::emit_get_value_preserving_key(const CompiledReference& reference) {
    if (reference.kind != ReferenceKind::computed_property) return emit_get_value(reference);

    // A computed Reference carries the evaluated property expression.  For a
    // read-modify-write operation, ToPropertyKey must happen at most once and
    // the resulting key must be reused by PutValue.
    builder_.emit_local(bytecode::OpCode::get_local, reference.base_slot);
    builder_.emit_local(bytecode::OpCode::get_local, reference.key_slot);
    builder_.emit(bytecode::OpCode::get_element_reference);

    // GET_ELEMENT_REFERENCE leaves [convertedKey, value].  Preserve the value,
    // replace the Reference's raw key with the converted PropertyKey value, and
    // restore the value as the expression result.
    const auto value_slot = scopes_->allocate_temporary();
    builder_.emit_local(bytecode::OpCode::set_local, value_slot);
    builder_.emit(bytecode::OpCode::pop);
    builder_.emit_local(bytecode::OpCode::set_local, reference.key_slot);
    builder_.emit(bytecode::OpCode::pop);
    builder_.emit_local(bytecode::OpCode::get_local, value_slot);
    return {};
}

Result<void> Compiler::emit_put_value(const CompiledReference& reference) {
    if (reference.kind == ReferenceKind::environment) {
        if (!reference.binding) return Error{ErrorCode::internal, "environment reference is missing binding metadata"};
        emit_set(*reference.binding);
        return {};
    }
    if (reference.kind == ReferenceKind::runtime_environment) {
        builder_.emit_name(reference.strict ? bytecode::OpCode::set_name_strict : bytecode::OpCode::set_name, reference.name_constant);
        return {};
    }

    const auto value_slot = scopes_->allocate_temporary();
    builder_.emit_local(bytecode::OpCode::set_local, value_slot);
    builder_.emit(bytecode::OpCode::pop);
    builder_.emit_local(bytecode::OpCode::get_local, reference.base_slot);

    if (reference.kind == ReferenceKind::computed_property) {
        builder_.emit_local(bytecode::OpCode::get_local, reference.key_slot);
        builder_.emit_local(bytecode::OpCode::get_local, value_slot);
        builder_.emit(reference.strict ? bytecode::OpCode::set_element_strict : bytecode::OpCode::set_element);
        return {};
    }

    builder_.emit_local(bytecode::OpCode::get_local, value_slot);
    builder_.emit_property(reference.strict ? bytecode::OpCode::set_property_strict : bytecode::OpCode::set_property, reference.key_constant);
    return {};
}

void Compiler::emit_get_this_value(const CompiledReference& reference) {
    if (!is_property_reference(reference)) {
        builder_.emit(bytecode::OpCode::undefined);
        return;
    }
    builder_.emit_local(bytecode::OpCode::get_local, reference.base_slot);
}

bool Compiler::is_property_reference(const CompiledReference& reference) const noexcept {
    return reference.kind == ReferenceKind::static_property || reference.kind == ReferenceKind::computed_property;
}

bool Compiler::is_unresolvable_reference(const frontend::ASTNode& target) const noexcept {
    if (const auto* identifier = as_identifier(&target)) {
        if (scopes_->find_binding(identifier->name) != nullptr) return false;
        return parent_ == nullptr || !parent_->resolve_capture(identifier->name).has_value();
    }
    return false;
}

Result<void> Compiler::emit_number_constant(const frontend::NumberLiteralNode& number) {
    const std::string text(number.value);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || errno == ERANGE) return error_at(number, "invalid numeric literal '" + text + "'");
    const auto constant = builder_.add_constant(context_->number(parsed));
    if (!constant) return constant.error();
    builder_.emit_constant(*constant);
    return {};
}

Result<void> Compiler::compile_binary(const frontend::BinaryExprNode& binary) {
    const auto left = compile_expression(*binary.left); if (!left) return left.error();
    const auto right = compile_expression(*binary.right); if (!right) return right.error();
    switch (binary.op) {
    case frontend::TokenKind::PLUS: builder_.emit(bytecode::OpCode::add); return {};
    case frontend::TokenKind::MINUS: builder_.emit(bytecode::OpCode::subtract); return {};
    case frontend::TokenKind::STAR: builder_.emit(bytecode::OpCode::multiply); return {};
    case frontend::TokenKind::SLASH: builder_.emit(bytecode::OpCode::divide); return {};
    case frontend::TokenKind::PERCENT: builder_.emit(bytecode::OpCode::remainder); return {};
    case frontend::TokenKind::STAR_STAR: builder_.emit(bytecode::OpCode::exponentiate); return {};
    case frontend::TokenKind::LESS: builder_.emit(bytecode::OpCode::less); return {};
    case frontend::TokenKind::LESS_EQUAL: builder_.emit(bytecode::OpCode::less_equal); return {};
    case frontend::TokenKind::GREATER: builder_.emit(bytecode::OpCode::greater); return {};
    case frontend::TokenKind::GREATER_EQUAL: builder_.emit(bytecode::OpCode::greater_equal); return {};
    case frontend::TokenKind::EQUAL_EQUAL: builder_.emit(bytecode::OpCode::equal); return {};
    case frontend::TokenKind::BANG_EQUAL: builder_.emit(bytecode::OpCode::not_equal); return {};
    case frontend::TokenKind::EQUAL_EQUAL_EQUAL: builder_.emit(bytecode::OpCode::strict_equal); return {};
    case frontend::TokenKind::BANG_EQUAL_EQUAL: builder_.emit(bytecode::OpCode::strict_not_equal); return {};
    case frontend::TokenKind::AMPERSAND: builder_.emit(bytecode::OpCode::bitwise_and); return {};
    case frontend::TokenKind::PIPE: builder_.emit(bytecode::OpCode::bitwise_or); return {};
    case frontend::TokenKind::CARET: builder_.emit(bytecode::OpCode::bitwise_xor); return {};
    case frontend::TokenKind::SHIFT_LEFT: builder_.emit(bytecode::OpCode::shift_left); return {};
    case frontend::TokenKind::SHIFT_RIGHT: builder_.emit(bytecode::OpCode::shift_right); return {};
    case frontend::TokenKind::SHIFT_RIGHT_UNSIGNED: builder_.emit(bytecode::OpCode::shift_right_unsigned); return {};
    case frontend::TokenKind::IN: builder_.emit(bytecode::OpCode::in_operator); return {};
    case frontend::TokenKind::INSTANCEOF: builder_.emit(bytecode::OpCode::instanceof_operator); return {};
    default: return error_at(binary, "binary operator is parsed but not executable");
    }
}

Result<void> Compiler::compile_logical(const frontend::LogicalExprNode& logical) {
    const auto left = compile_expression(*logical.left);
    if (!left) return left.error();

    const auto saved_slot = scopes_->allocate_temporary();
    builder_.emit_local(bytecode::OpCode::set_local, saved_slot);

    if (logical.op == frontend::TokenKind::AND_AND) {
        const auto short_circuit = builder_.emit_jump(bytecode::OpCode::jump_if_false);
        const auto right = compile_expression(*logical.right);
        if (!right) return right.error();
        const auto end_jump = builder_.emit_jump(bytecode::OpCode::jump);
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(logical, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(short_circuit, static_cast<std::uint32_t>(builder_.offset()));
        builder_.emit_local(bytecode::OpCode::get_local, saved_slot);
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(logical, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(end_jump, static_cast<std::uint32_t>(builder_.offset()));
        return {};
    }

    if (logical.op == frontend::TokenKind::NULLISH) {
        builder_.emit(bytecode::OpCode::is_nullish);
        const auto use_left = builder_.emit_jump(bytecode::OpCode::jump_if_false);
        const auto right = compile_expression(*logical.right);
        if (!right) return right.error();
        const auto end_jump = builder_.emit_jump(bytecode::OpCode::jump);
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(logical, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(use_left, static_cast<std::uint32_t>(builder_.offset()));
        builder_.emit_local(bytecode::OpCode::get_local, saved_slot);
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(logical, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(end_jump, static_cast<std::uint32_t>(builder_.offset()));
        return {};
    }

    if (logical.op == frontend::TokenKind::OR_OR) {
        // JUMP_IF_FALSE only branches on falsy values. Invert a copy of the truth test while
        // preserving the original operand in the hidden local so `||` returns values, not bools.
        builder_.emit(bytecode::OpCode::logical_not);
        const auto short_circuit = builder_.emit_jump(bytecode::OpCode::jump_if_false);
        const auto right = compile_expression(*logical.right);
        if (!right) return right.error();
        const auto end_jump = builder_.emit_jump(bytecode::OpCode::jump);
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(logical, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(short_circuit, static_cast<std::uint32_t>(builder_.offset()));
        builder_.emit_local(bytecode::OpCode::get_local, saved_slot);
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(logical, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(end_jump, static_cast<std::uint32_t>(builder_.offset()));
        return {};
    }

    return error_at(logical, "logical operator is parsed but not executable");
}

Result<void> Compiler::compile_unary(const frontend::UnaryExprNode& unary) {
    if (unary.op == frontend::TokenKind::TYPEOF) {
        if (const auto* identifier = as_identifier(unary.argument.get()); identifier != nullptr && is_unresolvable_reference(*identifier)) {
            const auto name = add_property_key(identifier->name);
            if (!name) return name.error();
            builder_.emit_name(bytecode::OpCode::get_name_or_undefined, *name);
        } else {
            const auto operand = compile_expression(*unary.argument); if (!operand) return operand.error();
        }
        builder_.emit(bytecode::OpCode::typeof_);
        return {};
    }

    if (unary.op == frontend::TokenKind::VOID) {
        const auto operand = compile_expression(*unary.argument); if (!operand) return operand.error();
        builder_.emit(bytecode::OpCode::pop);
        builder_.emit(bytecode::OpCode::undefined);
        return {};
    }

    if (unary.op == frontend::TokenKind::DELETE) {
        if (const auto* identifier = as_identifier(unary.argument.get())) {
            if (strict_) return error_at(unary, "delete of an unqualified identifier is not permitted in strict code");
            bool resolvable = scopes_->find_binding(identifier->name) != nullptr;
            if (!resolvable && parent_ != nullptr) resolvable = parent_->resolve_capture(identifier->name).has_value();
            if (!resolvable) {
                const auto global = context_->get_global(identifier->name);
                resolvable = static_cast<bool>(global);
            }
            const auto constant = builder_.add_constant(Value::boolean(!resolvable));
            if (!constant) return constant.error();
            builder_.emit_constant(*constant);
            return {};
        }

        if (unary.argument->type == frontend::ASTNodeType::MEMBER_EXPR) {
            const auto reference = compile_reference(*unary.argument);
            if (!reference) return reference.error();
            if (reference->kind == ReferenceKind::static_property) {
                builder_.emit_local(bytecode::OpCode::get_local, reference->base_slot);
                builder_.emit_property(strict_ ? bytecode::OpCode::delete_property_strict : bytecode::OpCode::delete_property, reference->key_constant);
                return {};
            }
            if (reference->kind == ReferenceKind::computed_property) {
                builder_.emit_local(bytecode::OpCode::get_local, reference->base_slot);
                builder_.emit_local(bytecode::OpCode::get_local, reference->key_slot);
                builder_.emit(strict_ ? bytecode::OpCode::delete_element_strict : bytecode::OpCode::delete_element);
                return {};
            }
            return error_at(unary, "invalid delete reference");
        }

        if (unary.argument->type == frontend::ASTNodeType::OPTIONAL_CHAIN_EXPR) {
            const auto& chain = static_cast<const frontend::OptionalChainExprNode&>(*unary.argument);
            if (!chain.segments.empty()) {
                const auto kind = chain.segments.back().kind;
                if (kind == frontend::OptionalChainSegmentKind::STATIC_PROPERTY || kind == frontend::OptionalChainSegmentKind::COMPUTED_PROPERTY)
                    return compile_optional_chain(chain, true);
            }
        }

        const auto operand = compile_expression(*unary.argument); if (!operand) return operand.error();
        builder_.emit(bytecode::OpCode::pop);
        const auto constant = builder_.add_constant(Value::boolean(true));
        if (!constant) return constant.error();
        builder_.emit_constant(*constant);
        return {};
    }

    const auto operand = compile_expression(*unary.argument); if (!operand) return operand.error();
    switch (unary.op) {
    case frontend::TokenKind::MINUS: builder_.emit(bytecode::OpCode::negate); return {};
    case frontend::TokenKind::PLUS: builder_.emit(bytecode::OpCode::positive); return {};
    case frontend::TokenKind::BANG: builder_.emit(bytecode::OpCode::logical_not); return {};
    case frontend::TokenKind::TILDE: builder_.emit(bytecode::OpCode::bitwise_not); return {};
    default: return error_at(unary, "unary operator is parsed but not executable");
    }
}

Result<void> Compiler::compile_conditional(const frontend::ConditionalExprNode& conditional) {
    const auto test = compile_expression(*conditional.test); if (!test) return test.error();
    const auto false_jump = builder_.emit_jump(bytecode::OpCode::jump_if_false);
    const auto consequent = compile_expression(*conditional.consequent); if (!consequent) return consequent.error();
    const auto end_jump = builder_.emit_jump(bytecode::OpCode::jump);
    if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(conditional, "bytecode offset exceeds 32-bit jump range");
    builder_.patch_jump(false_jump, static_cast<std::uint32_t>(builder_.offset()));
    const auto alternate = compile_expression(*conditional.alternate); if (!alternate) return alternate.error();
    if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(conditional, "bytecode offset exceeds 32-bit jump range");
    builder_.patch_jump(end_jump, static_cast<std::uint32_t>(builder_.offset()));
    return {};
}

Result<void> Compiler::compile_sequence(const frontend::SequenceExprNode& sequence) {
    if (sequence.expressions.empty()) return error_at(sequence, "empty sequence expression");
    for (std::size_t index = 0; index < sequence.expressions.size(); ++index) {
        const auto value = compile_expression(*sequence.expressions[index]); if (!value) return value.error();
        if (index + 1U != sequence.expressions.size()) builder_.emit(bytecode::OpCode::pop);
    }
    return {};
}

Result<void> Compiler::compile_update(const frontend::UpdateExprNode& update) {
    const bool increment = update.op == frontend::TokenKind::PLUS_PLUS;
    if (!increment && update.op != frontend::TokenKind::MINUS_MINUS)
        return error_at(update, "update operator is parsed but not executable");

    const auto reference = compile_reference(*update.argument);
    if (!reference) return reference.error();

    const auto read = emit_get_value_preserving_key(*reference);
    if (!read) return read.error();
    builder_.emit(bytecode::OpCode::positive);

    std::optional<std::uint32_t> old_slot;
    if (!update.prefix) {
        old_slot = scopes_->allocate_temporary();
        builder_.emit_local(bytecode::OpCode::set_local, *old_slot);
    }

    const auto one = builder_.add_constant(Value::number(1.0));
    if (!one) return one.error();
    builder_.emit_constant(*one);
    builder_.emit(increment ? bytecode::OpCode::add : bytecode::OpCode::subtract);

    const auto write = emit_put_value(*reference);
    if (!write) return write.error();

    if (!update.prefix) {
        builder_.emit(bytecode::OpCode::pop);
        builder_.emit_local(bytecode::OpCode::get_local, *old_slot);
    }
    return {};
}
Result<void> Compiler::compile_yield(const frontend::YieldExprNode& expression) {
    if (!in_generator_) return error_at(expression, "yield is only executable inside a generator function");
    if (expression.argument) {
        const auto value = compile_expression(*expression.argument); if (!value) return value.error();
    } else {
        builder_.emit(bytecode::OpCode::undefined);
    }
    builder_.emit(bytecode::OpCode::yield_);
    return {};
}

Result<void> Compiler::compile_assignment(const frontend::AssignmentExprNode& assignment) {
    if (assignment.op == frontend::TokenKind::EQUAL &&
        (assignment.left->type == frontend::ASTNodeType::ARRAY_PATTERN || assignment.left->type == frontend::ASTNodeType::OBJECT_PATTERN)) {
        const auto rhs = compile_expression(*assignment.right); if (!rhs) return rhs.error();
        const auto value_slot = scopes_->allocate_temporary();
        builder_.emit_local(bytecode::OpCode::set_local, value_slot); builder_.emit(bytecode::OpCode::pop);
        const auto assigned = compile_assignment_pattern(*assignment.left, value_slot); if (!assigned) return assigned.error();
        builder_.emit_local(bytecode::OpCode::get_local, value_slot);
        return {};
    }

    const auto reference = compile_reference(*assignment.left);
    if (!reference) return reference.error();

    if (assignment.op == frontend::TokenKind::EQUAL) {
        const auto rhs = compile_expression(*assignment.right);
        if (!rhs) return rhs.error();
        return emit_put_value(*reference);
    }

    if (is_logical_assignment(assignment.op)) {
        // Preserve both the Reference and its current value across RHS evaluation.
        // compile_reference() has already captured a computed base/key exactly once.
        const auto read = emit_get_value_preserving_key(*reference);
        if (!read) return read.error();

        const auto old_value_slot = scopes_->allocate_temporary();
        builder_.emit_local(bytecode::OpCode::set_local, old_value_slot);
        builder_.emit(bytecode::OpCode::pop);

        builder_.emit_local(bytecode::OpCode::get_local, old_value_slot);
        if (assignment.op == frontend::TokenKind::OR_OR_EQUAL) {
            // JUMP_IF_FALSE should skip the assignment when the original value is truthy.
            builder_.emit(bytecode::OpCode::logical_not);
        } else if (assignment.op == frontend::TokenKind::NULLISH_EQUAL) {
            // Nullish assignment is intentionally distinct from falsiness.
            builder_.emit(bytecode::OpCode::is_nullish);
        }

        const auto skip_assignment = builder_.emit_jump(bytecode::OpCode::jump_if_false);

        const auto rhs = compile_expression(*assignment.right);
        if (!rhs) return rhs.error();
        const auto write = emit_put_value(*reference);
        if (!write) return write.error();
        const auto end = builder_.emit_jump(bytecode::OpCode::jump);

        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            return error_at(assignment, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(skip_assignment, static_cast<std::uint32_t>(builder_.offset()));
        builder_.emit_local(bytecode::OpCode::get_local, old_value_slot);

        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            return error_at(assignment, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(end, static_cast<std::uint32_t>(builder_.offset()));
        return {};
    }

    const auto opcode = compound_assignment_opcode(assignment.op);
    if (!opcode) return error_at(assignment, "unsupported assignment operator");

    const auto read = emit_get_value_preserving_key(*reference);
    if (!read) return read.error();
    const auto rhs = compile_expression(*assignment.right);
    if (!rhs) return rhs.error();
    builder_.emit(*opcode);
    return emit_put_value(*reference);
}
Result<void> Compiler::compile_member(const frontend::MemberExprNode& member) {
    const auto reference = compile_reference(member);
    if (!reference) return reference.error();
    return emit_get_value(*reference);
}
Result<void> Compiler::compile_array(const frontend::ArrayExprNode& array) {
    builder_.emit(bytecode::OpCode::new_array);
    for (const auto& element : array.elements) {
        if (!element) { builder_.emit(bytecode::OpCode::append_hole); continue; }
        if (element->type == frontend::ASTNodeType::SPREAD_ELEMENT) {
            const auto& spread = static_cast<const frontend::SpreadElementNode&>(*element);
            const auto r = compile_expression(*spread.argument); if (!r) return r.error();
            builder_.emit(bytecode::OpCode::append_spread);
        } else {
            const auto r = compile_expression(*element); if (!r) return r.error();
            builder_.emit(bytecode::OpCode::append_element);
        }
    }
    return {};
}

Result<void> Compiler::compile_object(const frontend::ObjectExprNode& object) {
    builder_.emit(bytecode::OpCode::new_object);
    for (const auto& entry : object.properties) {
        if (entry->type == frontend::ASTNodeType::SPREAD_ELEMENT) {
            const auto& spread = static_cast<const frontend::SpreadElementNode&>(*entry);
            const auto value = compile_expression(*spread.argument); if (!value) return value.error();
            builder_.emit(bytecode::OpCode::copy_data_properties);
            continue;
        }
        if (entry->type != frontend::ASTNodeType::PROPERTY) return error_at(*entry, "invalid object literal entry");
        const auto& property = static_cast<const frontend::PropertyNode&>(*entry);
        const bool getter = property.kind == frontend::PropertyKind::Getter;
        if (property.kind == frontend::PropertyKind::Setter)
            return error_at(property, "object literal setters are not implemented");
        if (property.computed) {
            const auto key = compile_expression(*property.key); if (!key) return key.error();
            if (getter) {
                if (property.value->type != frontend::ASTNodeType::FUNCTION_EXPR) return error_at(property, "getter value must be a function");
                const auto& function = static_cast<const frontend::FunctionExpressionNode&>(*property.value);
                const auto function_value = compile_function_value(function, false); if (!function_value) return function_value.error();
                const auto constant = builder_.add_constant(*function_value); if (!constant) return constant.error();
                builder_.emit_closure(*constant);
                builder_.emit(bytecode::OpCode::define_getter_element);
            } else {
                const auto value = compile_expression(*property.value); if (!value) return value.error();
                builder_.emit(bytecode::OpCode::define_element);
            }
            continue;
        }
        std::string key_text;
        if (property.key->type == frontend::ASTNodeType::IDENTIFIER) key_text = std::string(static_cast<const frontend::IdentifierNode&>(*property.key).name);
        else if (property.key->type == frontend::ASTNodeType::STRING_LITERAL) {
            const auto raw = static_cast<const frontend::StringLiteralNode&>(*property.key).value;
            key_text = raw.size() >= 2 ? std::string(raw.substr(1, raw.size() - 2)) : std::string(raw);
        } else if (property.key->type == frontend::ASTNodeType::NUMBER_LITERAL) key_text = std::string(static_cast<const frontend::NumberLiteralNode&>(*property.key).value);
        else return error_at(property, "unsupported object literal property key");
        if (getter) {
            if (property.value->type != frontend::ASTNodeType::FUNCTION_EXPR) return error_at(property, "getter value must be a function");
            const auto& function = static_cast<const frontend::FunctionExpressionNode&>(*property.value);
            const auto function_value = compile_function_value(function, false); if (!function_value) return function_value.error();
            const auto constant = builder_.add_constant(*function_value); if (!constant) return constant.error();
            builder_.emit_closure(*constant);
        } else {
            const auto value = compile_expression(*property.value); if (!value) return value.error();
        }
        const auto key = add_property_key(key_text); if (!key) return key.error();
        builder_.emit_property(getter ? bytecode::OpCode::define_getter : bytecode::OpCode::define_property, *key);
    }
    return {};
}

Result<void> Compiler::compile_argument_array(const std::vector<std::unique_ptr<frontend::ASTNode>>& arguments) {
    builder_.emit(bytecode::OpCode::new_array);
    for (const auto& argument : arguments) {
        if (argument->type == frontend::ASTNodeType::SPREAD_ELEMENT) {
            const auto& spread = static_cast<const frontend::SpreadElementNode&>(*argument);
            const auto r = compile_expression(*spread.argument); if (!r) return r.error();
            builder_.emit(bytecode::OpCode::append_spread);
        } else {
            const auto r = compile_expression(*argument); if (!r) return r.error();
            builder_.emit(bytecode::OpCode::append_element);
        }
    }
    return {};
}

Result<void> Compiler::compile_call(const frontend::CallExprNode& call) {
    const bool has_spread = std::any_of(call.arguments.begin(), call.arguments.end(), [](const auto& a){ return a->type == frontend::ASTNodeType::SPREAD_ELEMENT; });
    if (call.callee && call.callee->type == frontend::ASTNodeType::MEMBER_EXPR) {
        const auto reference = compile_reference(*call.callee); if (!reference) return reference.error();
        if (!is_property_reference(*reference)) return error_at(*call.callee, "method call target is not a property reference");
        emit_get_this_value(*reference);
        if (reference->kind == ReferenceKind::computed_property) builder_.emit_local(bytecode::OpCode::get_local, reference->key_slot);
        if (has_spread) {
            const auto args = compile_argument_array(call.arguments); if (!args) return args.error();
            if (reference->kind == ReferenceKind::computed_property) builder_.emit(bytecode::OpCode::call_element_spread);
            else builder_.emit_method_call_spread(reference->key_constant);
        } else {
            for (const auto& argument : call.arguments) { const auto r=compile_expression(*argument); if(!r) return r.error(); }
            if (reference->kind == ReferenceKind::computed_property) builder_.emit_element_call(static_cast<std::uint32_t>(call.arguments.size()));
            else builder_.emit_method_call(reference->key_constant, static_cast<std::uint32_t>(call.arguments.size()));
        }
        return {};
    }
    const auto callee = compile_expression(*call.callee); if (!callee) return callee.error();
    if (has_spread) { const auto args=compile_argument_array(call.arguments); if(!args) return args.error(); builder_.emit(bytecode::OpCode::call_spread); }
    else { for (const auto& argument : call.arguments) { const auto r=compile_expression(*argument); if(!r) return r.error(); } builder_.emit_call(static_cast<std::uint32_t>(call.arguments.size())); }
    return {};
}

Result<void> Compiler::compile_new(const frontend::NewExprNode& expression) {
    const auto callee = compile_expression(*expression.callee); if (!callee) return callee.error();
    const bool has_spread = std::any_of(expression.arguments.begin(), expression.arguments.end(), [](const auto& a){ return a->type == frontend::ASTNodeType::SPREAD_ELEMENT; });
    if (has_spread) { const auto args=compile_argument_array(expression.arguments); if(!args) return args.error(); builder_.emit(bytecode::OpCode::construct_spread); }
    else { for (const auto& argument : expression.arguments) { const auto r=compile_expression(*argument); if(!r) return r.error(); } builder_.emit_construct(static_cast<std::uint32_t>(expression.arguments.size())); }
    return {};
}

Result<void> Compiler::compile_template(const frontend::TemplateLiteralNode& literal) {
    if (literal.quasis.empty()) return error_at(literal, "template literal has no quasis");
    auto emit_text = [&](std::string_view text) -> Result<void> { const auto c=builder_.add_constant(context_->string(text)); if(!c) return c.error(); builder_.emit_constant(*c); return {}; };
    auto first = emit_text(literal.quasis.front()->cooked); if (!first) return first.error();
    for (std::size_t i=0; i<literal.expressions.size(); ++i) {
        const auto e=compile_expression(*literal.expressions[i]); if(!e) return e.error(); builder_.emit(bytecode::OpCode::add);
        const auto q=emit_text(literal.quasis[i+1]->cooked); if(!q) return q.error(); builder_.emit(bytecode::OpCode::add);
    }
    return {};
}

Result<void> Compiler::compile_tagged_template(const frontend::TaggedTemplateExprNode& tagged) {
    Value cooked = context_->array();
    Value raw = context_->array();
    for (std::size_t i=0;i<tagged.quasi->quasis.size();++i) {
        const auto& cooked_text = tagged.quasi->quasis[i]->cooked;
        const auto& raw_text = tagged.quasi->quasis[i]->raw;
        auto a=context_->array_push(cooked, context_->string(cooked_text)); if(!a) return a.error();
        auto b=context_->array_push(raw, context_->string(raw_text)); if(!b) return b.error();
    }
    auto raw_set=context_->set_own_property(cooked, context_->property_key("raw"), raw); if(!raw_set) return raw_set.error();
    const auto template_constant=builder_.add_constant(cooked); if(!template_constant) return template_constant.error();
    const std::uint32_t argc=static_cast<std::uint32_t>(1U+tagged.quasi->expressions.size());
    if (tagged.tag->type == frontend::ASTNodeType::MEMBER_EXPR) {
        const auto reference=compile_reference(*tagged.tag); if(!reference) return reference.error();
        emit_get_this_value(*reference);
        if(reference->kind==ReferenceKind::computed_property) builder_.emit_local(bytecode::OpCode::get_local, reference->key_slot);
        builder_.emit_constant(*template_constant);
        for(const auto& e: tagged.quasi->expressions){ const auto r=compile_expression(*e); if(!r)return r.error(); }
        if(reference->kind==ReferenceKind::computed_property) builder_.emit_element_call(argc); else builder_.emit_method_call(reference->key_constant, argc);
    } else {
        const auto t=compile_expression(*tagged.tag); if(!t)return t.error(); builder_.emit_constant(*template_constant);
        for(const auto& e: tagged.quasi->expressions){ const auto r=compile_expression(*e); if(!r)return r.error(); }
        builder_.emit_call(argc);
    }
    return {};
}

Result<Value> Compiler::compile_method_value(const frontend::MethodDefinitionNode& method, std::string_view display_name) {
    if (method.async || method.generator) return error_at(method, "async/generator methods are deferred until later stages");
    if (method.params.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(method, "too many method parameters");

    Compiler nested(*context_, this);
    nested.used_ = true;
    nested.in_function_ = true;
    nested.strict_ = true; // class methods are always strict
    const bool simple = simple_parameter_list(method.params, method.param_defaults, method.rest_parameter);
    if (has_duplicate_parameter_names(method.params)) return error_at(method, "duplicate parameters are not permitted in strict methods");
    if (has_strict_restricted_parameter(method.params)) return error_at(method, "eval/arguments parameter is not permitted in strict code");
    const auto setup = nested.scopes_->begin_method(method); if (!setup) return setup.error();
    const auto parameters = nested.compile_parameter_initializers(method.params, method.param_defaults, method.rest_parameter); if (!parameters) return parameters.error();
    const auto body_scope = nested.scopes_->begin_function_body(method.body->body, !simple); if (!body_scope) return body_scope.error();
    const auto body = nested.compile_statement_list(method.body->body, false, true); if (!body) return body.error();
    nested.builder_.emit(bytecode::OpCode::undefined);
    nested.builder_.emit(bytecode::OpCode::return_);
    nested.builder_.set_local_count(nested.scopes_->local_count());
    nested.builder_.set_local_binding_states(nested.scopes_->local_binding_states());
    nested.builder_.set_local_binding_immutable(nested.scopes_->local_binding_immutable());
    nested.builder_.set_upvalues(std::move(nested.upvalues_));
    nested.builder_.set_module_binding_count(nested.module_binding_count_);
    auto chunk = std::move(nested.builder_).finish();
    return context_->runtime().make_function(context_->realm(), std::string(display_name), function_length(method.param_defaults, method.rest_parameter, method.params.size()), std::move(chunk), method.key->name == "constructor" ? ConstructorKind::Base : ConstructorKind::None, false, nested.scopes_->arguments_slot(), ThisMode::Strict, true, static_cast<std::uint32_t>(method.params.size()), simple, true, parameter_names(method.params));
}

Result<void> Compiler::compile_class_declaration(const frontend::ClassDeclarationNode& declaration) {
    if (declaration.super_class) return error_at(declaration, "class extends/super are deferred until a later stage");
    const auto binding = scopes_->binding_for_class_declaration(declaration.id->name, *declaration.id); if (!binding) return binding.error();

    const frontend::MethodDefinitionNode* constructor = nullptr;
    for (const auto& method : declaration.methods) {
        if (method->key->name == "constructor") {
            if (constructor != nullptr) return error_at(*method, "class has more than one constructor");
            constructor = method.get();
        }
    }

    Value constructor_prototype;
    if (constructor != nullptr) {
        const auto compiled = compile_method_value(*constructor, declaration.id->name); if (!compiled) return compiled.error();
        constructor_prototype = *compiled;
    } else {
        bytecode::BytecodeBuilder default_builder;
        default_builder.emit(bytecode::OpCode::undefined);
        default_builder.emit(bytecode::OpCode::return_);
        default_builder.set_local_count(1U);
        default_builder.set_local_binding_states({BindingState::InitializedMutable});
        default_builder.set_local_binding_immutable({false});
        constructor_prototype = context_->runtime().make_function(context_->realm(), std::string(declaration.id->name), 0U, std::move(default_builder).finish(), ConstructorKind::Base, false, std::nullopt, ThisMode::Strict, true);
    }

    const auto constructor_constant = builder_.add_constant(constructor_prototype); if (!constructor_constant) return constructor_constant.error();
    builder_.emit_closure(*constructor_constant);
    builder_.emit_local(bytecode::OpCode::initialize_local, binding.value()->slot);
    builder_.emit(bytecode::OpCode::pop);
    scopes_->mark_initialized(*binding.value());

    const auto prototype_key = add_property_key("prototype"); if (!prototype_key) return prototype_key.error();
    builder_.emit_local(bytecode::OpCode::get_local, binding.value()->slot);
    builder_.emit(bytecode::OpCode::new_object);
    builder_.emit_property(bytecode::OpCode::set_property, *prototype_key);
    builder_.emit(bytecode::OpCode::pop);

    const auto constructor_key = add_property_key("constructor"); if (!constructor_key) return constructor_key.error();
    builder_.emit_local(bytecode::OpCode::get_local, binding.value()->slot);
    builder_.emit_property(bytecode::OpCode::get_property, *prototype_key);
    builder_.emit_local(bytecode::OpCode::get_local, binding.value()->slot);
    builder_.emit_property(bytecode::OpCode::define_property, *constructor_key);
    builder_.emit(bytecode::OpCode::pop);

    for (const auto& method : declaration.methods) {
        if (method.get() == constructor) continue;
        const std::string display_name = std::string(declaration.id->name) + "." + std::string(method->key->name);
        const auto compiled = compile_method_value(*method, display_name); if (!compiled) return compiled.error();
        const auto method_constant = builder_.add_constant(*compiled); if (!method_constant) return method_constant.error();
        const auto method_key = add_property_key(method->key->name); if (!method_key) return method_key.error();
        builder_.emit_local(bytecode::OpCode::get_local, binding.value()->slot);
        builder_.emit_property(bytecode::OpCode::get_property, *prototype_key);
        builder_.emit_closure(*method_constant);
        builder_.emit_property(bytecode::OpCode::define_property, *method_key);
        builder_.emit(bytecode::OpCode::pop);
    }
    return {};
}

Result<void> Compiler::compile_optional_chain(const frontend::OptionalChainExprNode& chain, bool delete_final) {
    const auto current_slot = scopes_->allocate_temporary();
    const auto receiver_slot = scopes_->allocate_temporary();
    std::vector<std::size_t> short_circuit_jumps;

    bool has_receiver = false;
    if (chain.base->type == frontend::ASTNodeType::MEMBER_EXPR) {
        const auto reference = compile_reference(*chain.base);
        if (!reference) return reference.error();
        if (!is_property_reference(*reference)) return error_at(*chain.base, "optional-call base is not a property reference");
        emit_get_this_value(*reference);
        builder_.emit_local(bytecode::OpCode::set_local, receiver_slot);
        builder_.emit(bytecode::OpCode::pop);
        const auto value = emit_get_value(*reference);
        if (!value) return value.error();
        builder_.emit_local(bytecode::OpCode::set_local, current_slot);
        builder_.emit(bytecode::OpCode::pop);
        has_receiver = true;
    } else {
        const auto base = compile_expression(*chain.base);
        if (!base) return base.error();
        builder_.emit_local(bytecode::OpCode::set_local, current_slot);
        builder_.emit(bytecode::OpCode::pop);
    }

    auto emit_optional_guard = [&](bool optional) -> Result<void> {
        if (!optional) return {};
        builder_.emit_local(bytecode::OpCode::get_local, current_slot);
        builder_.emit(bytecode::OpCode::is_nullish);
        const auto proceed = builder_.emit_jump(bytecode::OpCode::jump_if_false);
        if (delete_final) {
            const auto truth = builder_.add_constant(Value::boolean(true));
            if (!truth) return truth.error();
            builder_.emit_constant(*truth);
        } else {
            builder_.emit(bytecode::OpCode::undefined);
        }
        short_circuit_jumps.push_back(builder_.emit_jump(bytecode::OpCode::jump));
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            return error_at(chain, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(proceed, static_cast<std::uint32_t>(builder_.offset()));
        return {};
    };

    for (std::size_t segment_index = 0; segment_index < chain.segments.size(); ++segment_index) {
        const auto& segment = chain.segments[segment_index];
        const bool final_segment = segment_index + 1U == chain.segments.size();
        const auto guard = emit_optional_guard(segment.optional);
        if (!guard) return guard.error();

        if (segment.kind == frontend::OptionalChainSegmentKind::STATIC_PROPERTY) {
            if (!segment.property || segment.property->type != frontend::ASTNodeType::IDENTIFIER)
                return error_at(chain, "invalid optional-chain property segment");
            const auto& identifier = static_cast<const frontend::IdentifierNode&>(*segment.property);
            const auto key = add_property_key(identifier.name); if (!key) return key.error();
            builder_.emit_local(bytecode::OpCode::get_local, current_slot);
            if (delete_final && final_segment) {
                builder_.emit_property(strict_ ? bytecode::OpCode::delete_property_strict : bytecode::OpCode::delete_property, *key);
                if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(chain, "bytecode offset exceeds 32-bit jump range");
                const auto end = static_cast<std::uint32_t>(builder_.offset());
                for (const auto jump : short_circuit_jumps) builder_.patch_jump(jump, end);
                return {};
            }
            builder_.emit_local(bytecode::OpCode::set_local, receiver_slot);
            builder_.emit_property(bytecode::OpCode::get_property, *key);
            builder_.emit_local(bytecode::OpCode::set_local, current_slot);
            builder_.emit(bytecode::OpCode::pop);
            has_receiver = true;
            continue;
        }

        if (segment.kind == frontend::OptionalChainSegmentKind::COMPUTED_PROPERTY) {
            if (!segment.property) return error_at(chain, "missing optional-chain computed property");
            builder_.emit_local(bytecode::OpCode::get_local, current_slot);
            if (delete_final && final_segment) {
                const auto key = compile_expression(*segment.property); if (!key) return key.error();
                builder_.emit(strict_ ? bytecode::OpCode::delete_element_strict : bytecode::OpCode::delete_element);
                if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(chain, "bytecode offset exceeds 32-bit jump range");
                const auto end = static_cast<std::uint32_t>(builder_.offset());
                for (const auto jump : short_circuit_jumps) builder_.patch_jump(jump, end);
                return {};
            }
            builder_.emit_local(bytecode::OpCode::set_local, receiver_slot);
            const auto key = compile_expression(*segment.property); if (!key) return key.error();
            builder_.emit(bytecode::OpCode::get_element);
            builder_.emit_local(bytecode::OpCode::set_local, current_slot);
            builder_.emit(bytecode::OpCode::pop);
            has_receiver = true;
            continue;
        }

        const bool has_spread = std::any_of(segment.arguments.begin(), segment.arguments.end(), [](const auto& argument) {
            return argument->type == frontend::ASTNodeType::SPREAD_ELEMENT;
        });
        builder_.emit_local(bytecode::OpCode::get_local, current_slot);
        if (has_receiver) builder_.emit_local(bytecode::OpCode::get_local, receiver_slot);
        else builder_.emit(bytecode::OpCode::undefined);
        if (has_spread) {
            const auto args = compile_argument_array(segment.arguments); if (!args) return args.error();
            builder_.emit_call_with_this_spread();
        } else {
            for (const auto& argument : segment.arguments) {
                const auto compiled = compile_expression(*argument); if (!compiled) return compiled.error();
            }
            builder_.emit_call_with_this(static_cast<std::uint32_t>(segment.arguments.size()));
        }
        builder_.emit_local(bytecode::OpCode::set_local, current_slot);
        builder_.emit(bytecode::OpCode::pop);
        has_receiver = false;
    }

    builder_.emit_local(bytecode::OpCode::get_local, current_slot);
    if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        return error_at(chain, "bytecode offset exceeds 32-bit jump range");
    const auto end = static_cast<std::uint32_t>(builder_.offset());
    for (const auto jump : short_circuit_jumps) builder_.patch_jump(jump, end);
    return {};
}

Result<void> Compiler::compile_expression(const frontend::ASTNode& node) {
    switch (node.type) {
    case frontend::ASTNodeType::STRING_LITERAL: { const auto& str=static_cast<const frontend::StringLiteralNode&>(node); if(str.value.size()<2U) return error_at(str,"invalid string literal"); auto text=str.value.substr(1U,str.value.size()-2U); const auto c=builder_.add_constant(context_->string(text)); if(!c) return c.error(); builder_.emit_constant(*c); return {}; }
    case frontend::ASTNodeType::NUMBER_LITERAL: return emit_number_constant(static_cast<const frontend::NumberLiteralNode&>(node));
    case frontend::ASTNodeType::BOOLEAN_LITERAL: {
        const auto& literal = static_cast<const frontend::BooleanLiteralNode&>(node);
        const auto constant = builder_.add_constant(Value::boolean(literal.value));
        if (!constant) return constant.error();
        builder_.emit_constant(*constant);
        return {};
    }
    case frontend::ASTNodeType::NULL_LITERAL: {
        const auto constant = builder_.add_constant(Value::null());
        if (!constant) return constant.error();
        builder_.emit_constant(*constant);
        return {};
    }
    case frontend::ASTNodeType::REGEXP_LITERAL: {
        const auto& regexp = static_cast<const frontend::RegExpLiteralNode&>(node);
        const auto value = context_->regexp(regexp.pattern, regexp.flags);
        if (!value) return error_at(regexp, value.error().message());
        const auto constant = builder_.add_constant(*value);
        if (!constant) return constant.error();
        builder_.emit_constant(*constant);
        return {};
    }
    case frontend::ASTNodeType::IDENTIFIER: {
        const auto& identifier = static_cast<const frontend::IdentifierNode&>(node);
        if (!is_unresolvable_reference(identifier)) {
            const auto binding = resolve_read(identifier.name, identifier);
            if (!binding) return binding.error();
            emit_get(*binding);
            return {};
        }
        const auto name = add_property_key(identifier.name);
        if (!name) return name.error();
        builder_.emit_name(bytecode::OpCode::get_name, *name);
        return {};
    }
    case frontend::ASTNodeType::THIS_EXPR:
        builder_.emit(bytecode::OpCode::get_this);
        return {};
    case frontend::ASTNodeType::UNARY_EXPR: return compile_unary(static_cast<const frontend::UnaryExprNode&>(node));
    case frontend::ASTNodeType::UPDATE_EXPR: return compile_update(static_cast<const frontend::UpdateExprNode&>(node));
    case frontend::ASTNodeType::YIELD_EXPR: return compile_yield(static_cast<const frontend::YieldExprNode&>(node));
    case frontend::ASTNodeType::BINARY_EXPR: return compile_binary(static_cast<const frontend::BinaryExprNode&>(node));
    case frontend::ASTNodeType::LOGICAL_EXPR: return compile_logical(static_cast<const frontend::LogicalExprNode&>(node));
    case frontend::ASTNodeType::CONDITIONAL_EXPR: return compile_conditional(static_cast<const frontend::ConditionalExprNode&>(node));
    case frontend::ASTNodeType::SEQUENCE_EXPR: return compile_sequence(static_cast<const frontend::SequenceExprNode&>(node));
    case frontend::ASTNodeType::ASSIGNMENT_EXPR: return compile_assignment(static_cast<const frontend::AssignmentExprNode&>(node));
    case frontend::ASTNodeType::CALL_EXPR: return compile_call(static_cast<const frontend::CallExprNode&>(node));
    case frontend::ASTNodeType::NEW_EXPR: return compile_new(static_cast<const frontend::NewExprNode&>(node));
    case frontend::ASTNodeType::FUNCTION_EXPR: return compile_function_expression(static_cast<const frontend::FunctionExpressionNode&>(node));
    case frontend::ASTNodeType::ARROW_FUNCTION_EXPR: return compile_arrow(static_cast<const frontend::ArrowFunctionExprNode&>(node));
    case frontend::ASTNodeType::MEMBER_EXPR: return compile_member(static_cast<const frontend::MemberExprNode&>(node));
    case frontend::ASTNodeType::ARRAY_EXPR: return compile_array(static_cast<const frontend::ArrayExprNode&>(node));
    case frontend::ASTNodeType::OBJECT_EXPR: return compile_object(static_cast<const frontend::ObjectExprNode&>(node));
    case frontend::ASTNodeType::TEMPLATE_LITERAL: return compile_template(static_cast<const frontend::TemplateLiteralNode&>(node));
    case frontend::ASTNodeType::TAGGED_TEMPLATE_EXPR: return compile_tagged_template(static_cast<const frontend::TaggedTemplateExprNode&>(node));
    case frontend::ASTNodeType::OPTIONAL_CHAIN_EXPR: return compile_optional_chain(static_cast<const frontend::OptionalChainExprNode&>(node));
    default: return error_at(node, "expression node is parsed but not executable");
    }
}

Result<void> Compiler::compile_pattern(const frontend::ASTNode& pattern, std::uint32_t value_slot,
                                      frontend::VariableKind kind, bool assignment, bool parameter_binding) {
    auto bind_value = [&](const frontend::ASTNode& target, std::uint32_t slot) -> Result<void> {
        if (target.type == frontend::ASTNodeType::IDENTIFIER) {
            const auto& identifier = static_cast<const frontend::IdentifierNode&>(target);
            builder_.emit_local(bytecode::OpCode::get_local, slot);
            if (assignment) {
                const auto reference = compile_reference(target); if (!reference) return reference.error();
                const auto put = emit_put_value(*reference); if (!put) return put.error();
                builder_.emit(bytecode::OpCode::pop);
                return {};
            }
            const detail::Binding* binding = nullptr;
            if (parameter_binding) binding = scopes_->find_binding(identifier.name);
            else {
                const auto resolved = scopes_->binding_for_declaration(kind, identifier.name, identifier); if (!resolved) return resolved.error();
                binding = *resolved;
            }
            if (binding == nullptr) return error_at(identifier, "binding metadata is missing for destructuring target");
            builder_.emit_local((parameter_binding || kind != frontend::VariableKind::VAR) ? bytecode::OpCode::initialize_local : bytecode::OpCode::set_local, binding->slot);
            builder_.emit(bytecode::OpCode::pop);
            return {};
        }
        if (assignment && target.type == frontend::ASTNodeType::MEMBER_EXPR) {
            const auto reference = compile_reference(target); if (!reference) return reference.error();
            builder_.emit_local(bytecode::OpCode::get_local, slot);
            const auto put = emit_put_value(*reference); if (!put) return put.error();
            builder_.emit(bytecode::OpCode::pop);
            return {};
        }
        return error_at(target, "invalid destructuring target");
    };

    if (pattern.type == frontend::ASTNodeType::IDENTIFIER || (assignment && pattern.type == frontend::ASTNodeType::MEMBER_EXPR))
        return bind_value(pattern, value_slot);

    if (pattern.type == frontend::ASTNodeType::ASSIGNMENT_PATTERN) {
        const auto& defaulted = static_cast<const frontend::AssignmentPatternNode&>(pattern);
        const auto selected = scopes_->allocate_temporary();
        builder_.emit_local(bytecode::OpCode::get_local, value_slot);
        builder_.emit(bytecode::OpCode::undefined);
        builder_.emit(bytecode::OpCode::strict_equal);
        const auto supplied = builder_.emit_jump(bytecode::OpCode::jump_if_false);
        const auto initializer = compile_expression(*defaulted.right); if (!initializer) return initializer.error();
        builder_.emit_local(bytecode::OpCode::set_local, selected); builder_.emit(bytecode::OpCode::pop);
        const auto done = builder_.emit_jump(bytecode::OpCode::jump);
        builder_.patch_jump(supplied, static_cast<std::uint32_t>(builder_.offset()));
        builder_.emit_local(bytecode::OpCode::get_local, value_slot);
        builder_.emit_local(bytecode::OpCode::set_local, selected); builder_.emit(bytecode::OpCode::pop);
        builder_.patch_jump(done, static_cast<std::uint32_t>(builder_.offset()));
        return compile_pattern(*defaulted.left, selected, kind, assignment, parameter_binding);
    }

    if (pattern.type == frontend::ASTNodeType::REST_ELEMENT)
        return compile_pattern(*static_cast<const frontend::RestElementNode&>(pattern).argument, value_slot, kind, assignment, parameter_binding);

    if (pattern.type == frontend::ASTNodeType::ARRAY_PATTERN) {
        const auto& array = static_cast<const frontend::ArrayPatternNode&>(pattern);
        builder_.emit_local(bytecode::OpCode::get_local, value_slot);
        const auto iterator_key = builder_.add_constant(context_->well_known_symbol("iterator")); if (!iterator_key) return iterator_key.error();
        builder_.emit_constant(*iterator_key); builder_.emit_element_call(0);
        const auto iterator_slot = scopes_->allocate_temporary();
        const auto result_slot = scopes_->allocate_temporary();
        builder_.emit_local(bytecode::OpCode::set_local, iterator_slot); builder_.emit(bytecode::OpCode::pop);
        const auto next_key = add_property_key("next"); if (!next_key) return next_key.error();
        const auto done_key = add_property_key("done"); if (!done_key) return done_key.error();
        const auto item_key = add_property_key("value"); if (!item_key) return item_key.error();

        auto emit_next_value = [&]() -> Result<std::uint32_t> {
            const auto item_slot = scopes_->allocate_temporary();
            builder_.emit_local(bytecode::OpCode::get_local, iterator_slot); builder_.emit_method_call(*next_key, 0);
            builder_.emit_local(bytecode::OpCode::set_local, result_slot); builder_.emit(bytecode::OpCode::pop);
            builder_.emit_local(bytecode::OpCode::get_local, result_slot); builder_.emit_property(bytecode::OpCode::get_property, *done_key);
            const auto has_value = builder_.emit_jump(bytecode::OpCode::jump_if_false);
            builder_.emit(bytecode::OpCode::undefined);
            builder_.emit_local(bytecode::OpCode::set_local, item_slot); builder_.emit(bytecode::OpCode::pop);
            const auto completed = builder_.emit_jump(bytecode::OpCode::jump);
            builder_.patch_jump(has_value, static_cast<std::uint32_t>(builder_.offset()));
            builder_.emit_local(bytecode::OpCode::get_local, result_slot); builder_.emit_property(bytecode::OpCode::get_property, *item_key);
            builder_.emit_local(bytecode::OpCode::set_local, item_slot); builder_.emit(bytecode::OpCode::pop);
            builder_.patch_jump(completed, static_cast<std::uint32_t>(builder_.offset()));
            return item_slot;
        };

        for (const auto& element : array.elements) {
            if (element && element->type == frontend::ASTNodeType::REST_ELEMENT) {
                const auto rest_array_slot = scopes_->allocate_temporary();
                builder_.emit(bytecode::OpCode::new_array); builder_.emit_local(bytecode::OpCode::set_local, rest_array_slot); builder_.emit(bytecode::OpCode::pop);
                const auto loop = static_cast<std::uint32_t>(builder_.offset());
                builder_.emit_local(bytecode::OpCode::get_local, iterator_slot); builder_.emit_method_call(*next_key, 0);
                builder_.emit_local(bytecode::OpCode::set_local, result_slot); builder_.emit(bytecode::OpCode::pop);
                builder_.emit_local(bytecode::OpCode::get_local, result_slot); builder_.emit_property(bytecode::OpCode::get_property, *done_key);
                builder_.emit(bytecode::OpCode::logical_not);
                const auto exit = builder_.emit_jump(bytecode::OpCode::jump_if_false);
                builder_.emit_local(bytecode::OpCode::get_local, rest_array_slot);
                builder_.emit_local(bytecode::OpCode::get_local, result_slot); builder_.emit_property(bytecode::OpCode::get_property, *item_key);
                builder_.emit(bytecode::OpCode::append_element); builder_.emit(bytecode::OpCode::pop);
                const auto back = builder_.emit_jump(bytecode::OpCode::jump); builder_.patch_jump(back, loop);
                builder_.patch_jump(exit, static_cast<std::uint32_t>(builder_.offset()));
                const auto r = compile_pattern(*element, rest_array_slot, kind, assignment, parameter_binding); if (!r) return r.error();
                break;
            }
            const auto item = emit_next_value(); if (!item) return item.error();
            if (!element) continue;
            const auto r = compile_pattern(*element, *item, kind, assignment, parameter_binding); if (!r) return r.error();
        }
        return {};
    }

    if (pattern.type == frontend::ASTNodeType::OBJECT_PATTERN) {
        const auto& object = static_cast<const frontend::ObjectPatternNode&>(pattern);
        const auto object_slot = scopes_->allocate_temporary();
        builder_.emit_local(bytecode::OpCode::get_local, value_slot); builder_.emit(bytecode::OpCode::to_object);
        builder_.emit_local(bytecode::OpCode::set_local, object_slot); builder_.emit(bytecode::OpCode::pop);
        std::optional<std::uint32_t> excluded_slot;
        if (object.rest) {
            excluded_slot = scopes_->allocate_temporary();
            builder_.emit(bytecode::OpCode::new_array); builder_.emit_local(bytecode::OpCode::set_local, *excluded_slot); builder_.emit(bytecode::OpCode::pop);
        }
        for (const auto& property : object.properties) {
            const auto property_value = scopes_->allocate_temporary();
            if (property->computed) {
                const auto key = compile_expression(*property->key); if (!key) return key.error();
                const auto key_slot = scopes_->allocate_temporary();
                builder_.emit_local(bytecode::OpCode::set_local, key_slot); builder_.emit(bytecode::OpCode::pop);
                if (excluded_slot) {
                    builder_.emit_local(bytecode::OpCode::get_local, *excluded_slot); builder_.emit_local(bytecode::OpCode::get_local, key_slot);
                    builder_.emit(bytecode::OpCode::append_element); builder_.emit(bytecode::OpCode::pop);
                }
                builder_.emit_local(bytecode::OpCode::get_local, object_slot); builder_.emit_local(bytecode::OpCode::get_local, key_slot); builder_.emit(bytecode::OpCode::get_element);
            } else {
                std::string key_text;
                if (property->key->type == frontend::ASTNodeType::IDENTIFIER) key_text = std::string(static_cast<const frontend::IdentifierNode&>(*property->key).name);
                else if (property->key->type == frontend::ASTNodeType::STRING_LITERAL) {
                    const auto raw = static_cast<const frontend::StringLiteralNode&>(*property->key).value;
                    key_text = raw.size() >= 2 ? std::string(raw.substr(1, raw.size()-2)) : std::string(raw);
                } else if (property->key->type == frontend::ASTNodeType::NUMBER_LITERAL) key_text = std::string(static_cast<const frontend::NumberLiteralNode&>(*property->key).value);
                else return error_at(*property, "unsupported object pattern property key");
                if (excluded_slot) {
                    const auto key_constant = builder_.add_constant(context_->string(key_text)); if (!key_constant) return key_constant.error();
                    builder_.emit_local(bytecode::OpCode::get_local, *excluded_slot); builder_.emit_constant(*key_constant);
                    builder_.emit(bytecode::OpCode::append_element); builder_.emit(bytecode::OpCode::pop);
                }
                const auto key = add_property_key(key_text); if (!key) return key.error();
                builder_.emit_local(bytecode::OpCode::get_local, object_slot); builder_.emit_property(bytecode::OpCode::get_property, *key);
            }
            builder_.emit_local(bytecode::OpCode::set_local, property_value); builder_.emit(bytecode::OpCode::pop);
            const auto r = compile_pattern(*property->value, property_value, kind, assignment, parameter_binding); if (!r) return r.error();
        }
        if (object.rest) {
            const auto rest_slot = scopes_->allocate_temporary();
            builder_.emit_local(bytecode::OpCode::get_local, object_slot); builder_.emit_local(bytecode::OpCode::get_local, *excluded_slot);
            builder_.emit(bytecode::OpCode::copy_object_rest);
            builder_.emit_local(bytecode::OpCode::set_local, rest_slot); builder_.emit(bytecode::OpCode::pop);
            const auto r = compile_pattern(*object.rest, rest_slot, kind, assignment, parameter_binding); if (!r) return r.error();
        }
        return {};
    }
    return error_at(pattern, "unsupported destructuring pattern");
}

Result<void> Compiler::compile_binding_pattern(const frontend::ASTNode& pattern, std::uint32_t value_slot,
                                               frontend::VariableKind kind, bool parameter_binding) {
    return compile_pattern(pattern, value_slot, kind, false, parameter_binding);
}

Result<void> Compiler::compile_assignment_pattern(const frontend::ASTNode& pattern, std::uint32_t value_slot) {
    return compile_pattern(pattern, value_slot, frontend::VariableKind::LET, true, false);
}

Result<void> Compiler::compile_variable_declaration(const frontend::VariableDeclarationNode& declaration) {
    for (const auto& declarator : declaration.declarations) {
        if (declarator->id->type == frontend::ASTNodeType::IDENTIFIER) {
            const auto& identifier = static_cast<const frontend::IdentifierNode&>(*declarator->id);
            const auto binding = scopes_->binding_for_declaration(declaration.kind, identifier.name, identifier); if (!binding) return binding.error();
            if (declaration.kind == frontend::VariableKind::VAR && !declarator->init) continue;
            if (declarator->init) {
                const auto initializer = compile_expression(*declarator->init); if (!initializer) return initializer.error();
            } else builder_.emit(bytecode::OpCode::undefined);
            builder_.emit_local(declaration.kind == frontend::VariableKind::VAR ? bytecode::OpCode::set_local : bytecode::OpCode::initialize_local, binding.value()->slot);
            builder_.emit(bytecode::OpCode::pop);
            scopes_->mark_initialized(*binding.value());
            continue;
        }
        if (!declarator->init) return error_at(*declarator, "destructuring declaration requires initializer");
        const auto initializer = compile_expression(*declarator->init); if (!initializer) return initializer.error();
        const auto value_slot = scopes_->allocate_temporary();
        builder_.emit_local(bytecode::OpCode::set_local, value_slot); builder_.emit(bytecode::OpCode::pop);
        const auto bound = compile_binding_pattern(*declarator->id, value_slot, declaration.kind); if (!bound) return bound.error();
    }
    return {};
}

Result<void> Compiler::compile_parameter_initializers(const std::vector<std::unique_ptr<frontend::ASTNode>>& params,
                                                      const std::vector<std::unique_ptr<frontend::ASTNode>>& defaults,
                                                      std::optional<std::size_t> rest_parameter) {
    const bool simple = simple_parameter_list(params, defaults, rest_parameter);
    if (simple) return {};
    for (std::size_t index = 0; index < params.size(); ++index) {
        const auto value_slot = scopes_->allocate_temporary();
        if (rest_parameter && *rest_parameter == index) builder_.emit_rest_arguments(static_cast<std::uint32_t>(index));
        else builder_.emit_argument(static_cast<std::uint32_t>(index));
        if (index < defaults.size() && defaults[index]) {
            builder_.emit_local(bytecode::OpCode::set_local, value_slot); builder_.emit(bytecode::OpCode::pop);
            builder_.emit_local(bytecode::OpCode::get_local, value_slot); builder_.emit(bytecode::OpCode::undefined); builder_.emit(bytecode::OpCode::strict_equal);
            const auto supplied = builder_.emit_jump(bytecode::OpCode::jump_if_false);
            const auto initializer = compile_expression(*defaults[index]); if (!initializer) return initializer.error();
            builder_.emit_local(bytecode::OpCode::set_local, value_slot); builder_.emit(bytecode::OpCode::pop);
            const auto done = builder_.emit_jump(bytecode::OpCode::jump);
            builder_.patch_jump(supplied, static_cast<std::uint32_t>(builder_.offset()));
            builder_.patch_jump(done, static_cast<std::uint32_t>(builder_.offset()));
        } else {
            builder_.emit_local(bytecode::OpCode::set_local, value_slot); builder_.emit(bytecode::OpCode::pop);
        }
        const auto bound = compile_binding_pattern(*params[index], value_slot, frontend::VariableKind::LET, true); if (!bound) return bound.error();
    }
    return {};
}

Result<void> Compiler::compile_arrow(const frontend::ArrowFunctionExprNode& arrow) {
    if (arrow.async) return error_at(arrow, "async arrow functions are deferred until P13");
    if (arrow.params.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        return error_at(arrow, "too many arrow function parameters");
    Compiler nested(*context_, this);
    nested.used_ = true;
    nested.in_function_ = true;
    const bool simple = simple_parameter_list(arrow.params, arrow.param_defaults, arrow.rest_parameter);
    if (has_duplicate_parameter_names(arrow.params)) return error_at(arrow, "duplicate parameters are not permitted in arrow functions");
    if (nested.strict_ && has_strict_restricted_parameter(arrow.params)) return error_at(arrow, "eval/arguments parameter is not permitted in strict code");
    if (!arrow.expression_body) {
        const auto& block = static_cast<const frontend::BlockStatementNode&>(*arrow.body);
        const bool own_strict = has_use_strict_directive(block.body);
        if (own_strict && !simple) return error_at(arrow, "use strict directive is not permitted with a non-simple parameter list");
        nested.strict_ = nested.strict_ || own_strict;
        if (nested.strict_ && has_strict_restricted_parameter(arrow.params)) return error_at(arrow, "eval/arguments parameter is not permitted in strict code");
    }
    const auto setup = nested.scopes_->begin_arrow(arrow); if (!setup) return setup.error();
    const auto parameters = nested.compile_parameter_initializers(arrow.params, arrow.param_defaults, arrow.rest_parameter); if (!parameters) return parameters.error();
    if (arrow.expression_body) {
        const auto value = nested.compile_expression(*arrow.body); if (!value) return value.error();
        nested.builder_.emit(bytecode::OpCode::return_);
    } else {
        const auto& block = static_cast<const frontend::BlockStatementNode&>(*arrow.body);
        const auto body_scope = nested.scopes_->begin_function_body(block.body, !simple); if (!body_scope) return body_scope.error();
        const auto body = nested.compile_statement_list(block.body, false, true); if (!body) return body.error();
        nested.builder_.emit(bytecode::OpCode::undefined);
        nested.builder_.emit(bytecode::OpCode::return_);
    }
    nested.builder_.set_local_count(nested.scopes_->local_count());
    nested.builder_.set_local_binding_states(nested.scopes_->local_binding_states());
    nested.builder_.set_local_binding_immutable(nested.scopes_->local_binding_immutable());
    nested.builder_.set_upvalues(std::move(nested.upvalues_));
    nested.builder_.set_module_binding_count(nested.module_binding_count_);
    auto chunk = std::move(nested.builder_).finish();
    const Value prototype = context_->runtime().make_function(context_->realm(), "", function_length(arrow.param_defaults, arrow.rest_parameter, arrow.params.size()),
        std::move(chunk), ConstructorKind::None, false, std::nullopt, ThisMode::Lexical, false,
        static_cast<std::uint32_t>(arrow.params.size()), simple, nested.strict_, parameter_names(arrow.params));
    const auto constant = builder_.add_constant(prototype); if (!constant) return constant.error();
    builder_.emit_closure(*constant);
    return {};
}

Result<Value> Compiler::compile_function_value(const frontend::FunctionExpressionNode& function, bool constructable) {
    if (function.async) return error_at(function, "async functions are deferred until the async suspension stage");
    if (function.params.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(function, "too many function parameters");

    Compiler nested(*context_, this);
    nested.used_ = true;
    nested.in_function_ = true;
    nested.in_generator_ = function.generator;
    const bool simple = simple_parameter_list(function.params, function.param_defaults, function.rest_parameter);
    const bool own_strict = has_use_strict_directive(function.body->body);
    if (own_strict && !simple) return error_at(function, "use strict directive is not permitted with a non-simple parameter list");
    nested.strict_ = nested.strict_ || own_strict;
    const bool duplicates = has_duplicate_parameter_names(function.params);
    if (duplicates && (!simple || nested.strict_)) return error_at(function, "duplicate parameters require a simple non-strict parameter list");
    if (nested.strict_ && has_strict_restricted_parameter(function.params)) return error_at(function, "eval/arguments parameter is not permitted in strict code");
    const auto setup = nested.scopes_->begin_function(function, duplicates); if (!setup) return setup.error();
    const auto parameters = nested.compile_parameter_initializers(function.params, function.param_defaults, function.rest_parameter); if (!parameters) return parameters.error();
    const auto body_scope = nested.scopes_->begin_function_body(function.body->body, !simple); if (!body_scope) return body_scope.error();
    const auto body = nested.compile_statement_list(function.body->body, false, true); if (!body) return body.error();
    nested.builder_.emit(bytecode::OpCode::undefined);
    nested.builder_.emit(bytecode::OpCode::return_);
    nested.builder_.set_local_count(nested.scopes_->local_count());
    nested.builder_.set_local_binding_states(nested.scopes_->local_binding_states());
    nested.builder_.set_local_binding_immutable(nested.scopes_->local_binding_immutable());
    nested.builder_.set_upvalues(std::move(nested.upvalues_));
    nested.builder_.set_module_binding_count(nested.module_binding_count_);
    auto chunk = std::move(nested.builder_).finish();
    const std::string name = function.id ? std::string(function.id->name) : std::string{};
    return context_->runtime().make_function(context_->realm(), name, function_length(function.param_defaults, function.rest_parameter, function.params.size()),
        std::move(chunk), (function.generator || !constructable) ? ConstructorKind::None : ConstructorKind::Base, function.generator, nested.scopes_->arguments_slot(),
        nested.strict_ ? ThisMode::Strict : ThisMode::Global, false, static_cast<std::uint32_t>(function.params.size()), simple, nested.strict_, parameter_names(function.params));
}

Result<void> Compiler::compile_function_expression(const frontend::FunctionExpressionNode& function) {
    const auto function_value = compile_function_value(function); if (!function_value) return function_value.error();
    const auto constant = builder_.add_constant(*function_value); if (!constant) return constant.error();
    builder_.emit_closure(*constant);
    return {};
}

Result<Value> Compiler::compile_function_value(const frontend::FunctionDeclarationNode& function) {
    if (function.async) return error_at(function, "async functions are deferred until the async suspension stage");
    if (function.params.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(function, "too many function parameters");

    Compiler nested(*context_, this);
    nested.used_ = true;
    nested.in_function_ = true;
    nested.in_generator_ = function.generator;
    const bool simple = simple_parameter_list(function.params, function.param_defaults, function.rest_parameter);
    const bool own_strict = has_use_strict_directive(function.body->body);
    if (own_strict && !simple) return error_at(function, "use strict directive is not permitted with a non-simple parameter list");
    nested.strict_ = nested.strict_ || own_strict;
    const bool duplicates = has_duplicate_parameter_names(function.params);
    if (duplicates && (!simple || nested.strict_)) return error_at(function, "duplicate parameters require a simple non-strict parameter list");
    if (nested.strict_ && has_strict_restricted_parameter(function.params)) return error_at(function, "eval/arguments parameter is not permitted in strict code");
    const auto setup = nested.scopes_->begin_function(function, duplicates); if (!setup) return setup.error();
    const auto parameters = nested.compile_parameter_initializers(function.params, function.param_defaults, function.rest_parameter); if (!parameters) return parameters.error();
    const auto body_scope = nested.scopes_->begin_function_body(function.body->body, !simple); if (!body_scope) return body_scope.error();
    const auto body = nested.compile_statement_list(function.body->body, false, true); if (!body) return body.error();
    nested.builder_.emit(bytecode::OpCode::undefined);
    nested.builder_.emit(bytecode::OpCode::return_);
    nested.builder_.set_local_count(nested.scopes_->local_count());
    nested.builder_.set_local_binding_states(nested.scopes_->local_binding_states());
    nested.builder_.set_local_binding_immutable(nested.scopes_->local_binding_immutable());
    nested.builder_.set_upvalues(std::move(nested.upvalues_));
    nested.builder_.set_module_binding_count(nested.module_binding_count_);
    auto chunk = std::move(nested.builder_).finish();
    return context_->runtime().make_function(context_->realm(), std::string(function.id->name), function_length(function.param_defaults, function.rest_parameter, function.params.size()), std::move(chunk), function.generator ? ConstructorKind::None : ConstructorKind::Base, function.generator, nested.scopes_->arguments_slot(), nested.strict_ ? ThisMode::Strict : ThisMode::Global, false, static_cast<std::uint32_t>(function.params.size()), simple, nested.strict_, parameter_names(function.params));
}

Result<void> Compiler::compile_function_declaration(const frontend::FunctionDeclarationNode& function) {
    const auto binding = scopes_->binding_for_function_declaration(function.id->name, *function.id); if (!binding) return binding.error();
    const auto function_value = compile_function_value(function); if (!function_value) return function_value.error();
    const auto constant = builder_.add_constant(*function_value); if (!constant) return constant.error();
    builder_.emit_closure(*constant);
    builder_.emit_local(bytecode::OpCode::set_local, binding.value()->slot);
    builder_.emit(bytecode::OpCode::pop);
    return {};
}

Result<void> Compiler::compile_return(const frontend::ReturnStatementNode& statement) {
    if (!in_function_) return error_at(statement, "return is only valid inside a function");
    if (statement.argument) {
        const auto value = compile_expression(*statement.argument); if (!value) return value.error();
    } else {
        builder_.emit(bytecode::OpCode::undefined);
    }
    builder_.emit(bytecode::OpCode::return_);
    return {};
}

Result<void> Compiler::compile_throw(const frontend::ThrowStatementNode& statement) {
    const auto value = compile_expression(*statement.argument); if (!value) return value.error();
    builder_.emit(bytecode::OpCode::throw_);
    return {};
}

Result<void> Compiler::compile_try(const frontend::TryStatementNode& statement) {
    struct ProtectedFinallyGuard final {
        std::size_t& depth;
        bool active;
        ProtectedFinallyGuard(std::size_t& value, bool enabled) : depth(value), active(enabled) { if (active) ++depth; }
        ~ProtectedFinallyGuard() { if (active) --depth; }
    } guard(protected_finally_depth_, statement.finalizer != nullptr);

    auto checked_offset = [&](const frontend::ASTNode& node) -> Result<std::uint32_t> {
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            return error_at(node, "bytecode offset exceeds 32-bit handler range");
        }
        return static_cast<std::uint32_t>(builder_.offset());
    };

    const auto try_start = checked_offset(statement); if (!try_start) return try_start.error();
    const auto try_body = compile_block(*statement.block); if (!try_body) return try_body.error();
    const auto try_end = checked_offset(statement); if (!try_end) return try_end.error();

    std::optional<std::size_t> jump_from_try;
    if (statement.handler || statement.finalizer) jump_from_try = builder_.emit_jump(bytecode::OpCode::jump);

    std::uint32_t catch_start = bytecode::no_handler_target;
    std::uint32_t catch_end = bytecode::no_handler_target;
    std::optional<std::size_t> jump_from_catch;

    if (statement.handler) {
        const auto start = checked_offset(*statement.handler); if (!start) return start.error();
        catch_start = *start;
        if (!statement.handler_param) return error_at(statement, "catch block is missing its binding parameter");
        const auto catch_scope = scopes_->begin_catch(*statement.handler_param, *statement.handler);
        if (!catch_scope) return catch_scope.error();
        const auto catch_value_slot = scopes_->allocate_temporary();
        builder_.emit_local(bytecode::OpCode::set_local, catch_value_slot); builder_.emit(bytecode::OpCode::pop);
        const auto catch_bind = compile_binding_pattern(*statement.handler_param, catch_value_slot, frontend::VariableKind::LET, true);
        if (!catch_bind) { scopes_->end_block(); return catch_bind.error(); }
        const auto body = compile_statement_list(statement.handler->body, false, false);
        scopes_->end_block();
        if (!body) return body.error();
        const auto end = checked_offset(*statement.handler); if (!end) return end.error();
        catch_end = *end;
        if (statement.finalizer) jump_from_catch = builder_.emit_jump(bytecode::OpCode::jump);
    }

    std::uint32_t finally_start = bytecode::no_handler_target;
    std::uint32_t finally_end = bytecode::no_handler_target;
    if (statement.finalizer) {
        const auto start = checked_offset(*statement.finalizer); if (!start) return start.error();
        finally_start = *start;
        if (jump_from_try) builder_.patch_jump(*jump_from_try, finally_start);
        if (jump_from_catch) builder_.patch_jump(*jump_from_catch, finally_start);
        const auto finalizer = compile_block(*statement.finalizer); if (!finalizer) return finalizer.error();
        builder_.emit_end_finally(finally_start);
        const auto end = checked_offset(*statement.finalizer); if (!end) return end.error();
        finally_end = *end;
    }

    const auto after = checked_offset(statement); if (!after) return after.error();
    if (!statement.finalizer && jump_from_try) builder_.patch_jump(*jump_from_try, *after);

    builder_.add_exception_handler(bytecode::ExceptionHandler{
        *try_start, *try_end, catch_start, catch_end, finally_start, finally_end});
    return {};
}

Result<void> Compiler::compile_if(const frontend::IfStatementNode& statement) {
    const auto test = compile_expression(*statement.test); if (!test) return test.error();
    const auto false_operand = builder_.emit_jump(bytecode::OpCode::jump_if_false);
    const auto consequent = compile_statement(*statement.consequent, false); if (!consequent) return consequent.error();

    if (statement.alternate) {
        const auto end_operand = builder_.emit_jump(bytecode::OpCode::jump);
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(statement, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(false_operand, static_cast<std::uint32_t>(builder_.offset()));
        const auto alternate = compile_statement(*statement.alternate, false); if (!alternate) return alternate.error();
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(statement, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(end_operand, static_cast<std::uint32_t>(builder_.offset()));
    } else {
        if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return error_at(statement, "bytecode offset exceeds 32-bit jump range");
        builder_.patch_jump(false_operand, static_cast<std::uint32_t>(builder_.offset()));
    }
    return {};
}

Result<void> Compiler::compile_while(const frontend::WhileStatementNode& statement, const std::vector<std::string>& labels) {
    if (builder_.offset() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        return error_at(statement, "bytecode offset exceeds 32-bit jump range");
    const auto loop_start = static_cast<std::uint32_t>(builder_.offset());
    const auto test = compile_expression(*statement.test); if (!test) return test.error();
    const auto exit_jump = builder_.emit_jump(bytecode::OpCode::jump_if_false);

    controls_.push_back(ControlContext{protected_finally_depth_, true, labels, {}, {}});
    const auto body = compile_statement(*statement.body, false);
    ControlContext control = std::move(controls_.back()); controls_.pop_back();
    if (!body) return body.error();

    for (const auto operand : control.continue_jumps) builder_.patch_jump(operand, loop_start);
    const auto back_edge = builder_.emit_jump(bytecode::OpCode::jump); builder_.patch_jump(back_edge, loop_start);
    const auto loop_end = static_cast<std::uint32_t>(builder_.offset());
    builder_.patch_jump(exit_jump, loop_end);
    for (const auto operand : control.break_jumps) builder_.patch_jump(operand, loop_end);
    return {};
}

Result<void> Compiler::compile_do_while(const frontend::DoWhileStatementNode& statement, const std::vector<std::string>& labels) {
    const auto body_start = static_cast<std::uint32_t>(builder_.offset());
    controls_.push_back(ControlContext{protected_finally_depth_, true, labels, {}, {}});
    const auto body = compile_statement(*statement.body, false);
    ControlContext control = std::move(controls_.back()); controls_.pop_back();
    if (!body) return body.error();
    const auto continue_target = static_cast<std::uint32_t>(builder_.offset());
    for (const auto operand : control.continue_jumps) builder_.patch_jump(operand, continue_target);
    const auto test = compile_expression(*statement.test); if (!test) return test.error();
    const auto exit_jump = builder_.emit_jump(bytecode::OpCode::jump_if_false);
    const auto repeat = builder_.emit_jump(bytecode::OpCode::jump); builder_.patch_jump(repeat, body_start);
    const auto end = static_cast<std::uint32_t>(builder_.offset()); builder_.patch_jump(exit_jump, end);
    for (const auto operand : control.break_jumps) builder_.patch_jump(operand, end);
    return {};
}

Result<void> Compiler::compile_for(const frontend::ForStatementNode& statement, const std::vector<std::string>& labels) {
    bool lexical_scope = false;
    if (statement.init && statement.init->type == frontend::ASTNodeType::VARIABLE_DECLARATION) {
        const auto& declaration = static_cast<const frontend::VariableDeclarationNode&>(*statement.init);
        if (declaration.kind != frontend::VariableKind::VAR) {
            const auto begun = scopes_->begin_for_declaration(declaration); if (!begun) return begun.error();
            lexical_scope = true;
        }
    }
    auto cleanup = [&] { if (lexical_scope) scopes_->end_block(); };
    if (statement.init) {
        Result<void> init{};
        if (statement.init->type == frontend::ASTNodeType::VARIABLE_DECLARATION) {
            const auto& declaration = static_cast<const frontend::VariableDeclarationNode&>(*statement.init);
            if (lexical_scope) {
                for (const auto& declarator : declaration.declarations) {
                    std::vector<const frontend::IdentifierNode*> identifiers;
                    collect_bound_identifiers(*declarator->id, identifiers);
                    for (const auto* identifier : identifiers) {
                        const auto binding = scopes_->binding_for_declaration(declaration.kind, identifier->name, *identifier);
                        if (!binding) { cleanup(); return binding.error(); }
                        builder_.emit_local(bytecode::OpCode::reset_local, binding.value()->slot);
                    }
                }
            }
            init = compile_variable_declaration(declaration);
        } else {
            init = compile_expression(*statement.init);
            if (init) builder_.emit(bytecode::OpCode::pop);
        }
        if (!init) { cleanup(); return init.error(); }
    }
    const auto loop_start = static_cast<std::uint32_t>(builder_.offset());
    std::optional<std::size_t> exit_jump;
    if (statement.test) {
        const auto test = compile_expression(*statement.test); if (!test) { cleanup(); return test.error(); }
        exit_jump = builder_.emit_jump(bytecode::OpCode::jump_if_false);
    }
    controls_.push_back(ControlContext{protected_finally_depth_, true, labels, {}, {}});
    const auto body = compile_statement(*statement.body, false);
    ControlContext control = std::move(controls_.back()); controls_.pop_back();
    if (!body) { cleanup(); return body.error(); }
    const auto continue_target = static_cast<std::uint32_t>(builder_.offset());
    for (const auto operand : control.continue_jumps) builder_.patch_jump(operand, continue_target);
    if (lexical_scope && statement.init && statement.init->type == frontend::ASTNodeType::VARIABLE_DECLARATION) {
        const auto& declaration = static_cast<const frontend::VariableDeclarationNode&>(*statement.init);
        for (const auto& declarator : declaration.declarations) {
            std::vector<const frontend::IdentifierNode*> identifiers;
            collect_bound_identifiers(*declarator->id, identifiers);
            for (const auto* identifier : identifiers) {
                const auto binding = scopes_->binding_for_declaration(declaration.kind, identifier->name, *identifier);
                if (!binding) { cleanup(); return binding.error(); }
                builder_.emit_local(bytecode::OpCode::clone_local_binding, binding.value()->slot);
            }
        }
    }
    if (statement.update) {
        const auto update = compile_expression(*statement.update); if (!update) { cleanup(); return update.error(); }
        builder_.emit(bytecode::OpCode::pop);
    }
    const auto repeat = builder_.emit_jump(bytecode::OpCode::jump); builder_.patch_jump(repeat, loop_start);
    const auto end = static_cast<std::uint32_t>(builder_.offset());
    if (exit_jump) builder_.patch_jump(*exit_jump, end);
    for (const auto operand : control.break_jumps) builder_.patch_jump(operand, end);
    cleanup();
    return {};
}

Result<void> Compiler::compile_iteration_binding(const frontend::ASTNode& left, std::uint32_t value_slot) {
    if (left.type == frontend::ASTNodeType::VARIABLE_DECLARATION) {
        const auto& declaration = static_cast<const frontend::VariableDeclarationNode&>(left);
        if (declaration.declarations.size() != 1) return error_at(left, "iteration declaration requires one binding pattern");
        const auto& pattern = *declaration.declarations.front()->id;
        if (declaration.kind != frontend::VariableKind::VAR) {
            std::vector<const frontend::IdentifierNode*> identifiers;
            collect_bound_identifiers(pattern, identifiers);
            for (const auto* identifier : identifiers) {
                const auto binding = scopes_->binding_for_declaration(declaration.kind, identifier->name, *identifier); if (!binding) return binding.error();
                builder_.emit_local(bytecode::OpCode::reset_local, binding.value()->slot);
            }
        }
        return compile_binding_pattern(pattern, value_slot, declaration.kind);
    }
    if (left.type == frontend::ASTNodeType::ARRAY_PATTERN || left.type == frontend::ASTNodeType::OBJECT_PATTERN)
        return compile_assignment_pattern(left, value_slot);
    const auto reference = compile_reference(left); if (!reference) return reference.error();
    builder_.emit_local(bytecode::OpCode::get_local, value_slot);
    const auto put = emit_put_value(*reference); if (!put) return put.error();
    builder_.emit(bytecode::OpCode::pop);
    return {};
}

static bool is_lexical_iteration_declaration(const frontend::ASTNode& left) {
    if (left.type != frontend::ASTNodeType::VARIABLE_DECLARATION) return false;
    return static_cast<const frontend::VariableDeclarationNode&>(left).kind != frontend::VariableKind::VAR;
}

Result<void> Compiler::compile_for_of(const frontend::ForOfStatementNode& statement, const std::vector<std::string>& labels) {
    const auto iterable = compile_expression(*statement.iterable); if (!iterable) return iterable.error();
    builder_.emit(bytecode::OpCode::get_iterator);
    const auto iterator_slot = scopes_->allocate_temporary();
    const auto next_method_slot = scopes_->allocate_temporary();
    const auto result_slot = scopes_->allocate_temporary();
    const auto value_slot = scopes_->allocate_temporary();
    builder_.emit_local(bytecode::OpCode::set_local, next_method_slot); builder_.emit(bytecode::OpCode::pop);
    builder_.emit_local(bytecode::OpCode::set_local, iterator_slot); builder_.emit(bytecode::OpCode::pop);

    bool lexical_scope = false;
    if (is_lexical_iteration_declaration(*statement.left)) {
        const auto begun = scopes_->begin_for_declaration(static_cast<const frontend::VariableDeclarationNode&>(*statement.left)); if (!begun) return begun.error();
        lexical_scope = true;
    }
    auto cleanup = [&] { if (lexical_scope) scopes_->end_block(); };
    const auto loop_start = static_cast<std::uint32_t>(builder_.offset());
    builder_.emit_local(bytecode::OpCode::get_local, iterator_slot);
    builder_.emit_local(bytecode::OpCode::get_local, next_method_slot);
    builder_.emit(bytecode::OpCode::iterator_next);
    builder_.emit_local(bytecode::OpCode::set_local, result_slot); builder_.emit(bytecode::OpCode::pop);
    builder_.emit_local(bytecode::OpCode::get_local, result_slot); builder_.emit(bytecode::OpCode::iterator_complete);
    const auto body_jump = builder_.emit_jump(bytecode::OpCode::jump_if_false);
    const auto end_jump = builder_.emit_jump(bytecode::OpCode::jump);
    builder_.patch_jump(body_jump, static_cast<std::uint32_t>(builder_.offset()));
    builder_.emit_local(bytecode::OpCode::get_local, result_slot); builder_.emit(bytecode::OpCode::iterator_value);
    builder_.emit_local(bytecode::OpCode::set_local, value_slot); builder_.emit(bytecode::OpCode::pop);
    const auto assigned = compile_iteration_binding(*statement.left, value_slot); if (!assigned) { cleanup(); return assigned.error(); }
    controls_.push_back(ControlContext{protected_finally_depth_, true, labels, {}, {}});
    const auto body = compile_statement(*statement.body, false);
    ControlContext control = std::move(controls_.back()); controls_.pop_back();
    if (!body) { cleanup(); return body.error(); }
    for (const auto operand : control.continue_jumps) builder_.patch_jump(operand, loop_start);
    const auto repeat = builder_.emit_jump(bytecode::OpCode::jump); builder_.patch_jump(repeat, loop_start);
    const auto end = static_cast<std::uint32_t>(builder_.offset()); builder_.patch_jump(end_jump, end);
    for (const auto operand : control.break_jumps) builder_.patch_jump(operand, end);
    cleanup();
    return {};
}

Result<void> Compiler::compile_for_in(const frontend::ForInStatementNode& statement, const std::vector<std::string>& labels) {
    const auto object = compile_expression(*statement.object); if (!object) return object.error();
    builder_.emit(bytecode::OpCode::enumerate_keys);
    builder_.emit(bytecode::OpCode::get_iterator);
    const auto iterator_slot = scopes_->allocate_temporary();
    const auto next_method_slot = scopes_->allocate_temporary();
    const auto result_slot = scopes_->allocate_temporary();
    const auto value_slot = scopes_->allocate_temporary();
    builder_.emit_local(bytecode::OpCode::set_local, next_method_slot); builder_.emit(bytecode::OpCode::pop);
    builder_.emit_local(bytecode::OpCode::set_local, iterator_slot); builder_.emit(bytecode::OpCode::pop);
    bool lexical_scope = false;
    if (is_lexical_iteration_declaration(*statement.left)) {
        const auto begun = scopes_->begin_for_declaration(static_cast<const frontend::VariableDeclarationNode&>(*statement.left)); if (!begun) return begun.error();
        lexical_scope = true;
    }
    auto cleanup = [&] { if (lexical_scope) scopes_->end_block(); };
    const auto loop_start = static_cast<std::uint32_t>(builder_.offset());
    builder_.emit_local(bytecode::OpCode::get_local, iterator_slot);
    builder_.emit_local(bytecode::OpCode::get_local, next_method_slot);
    builder_.emit(bytecode::OpCode::iterator_next);
    builder_.emit_local(bytecode::OpCode::set_local, result_slot); builder_.emit(bytecode::OpCode::pop);
    builder_.emit_local(bytecode::OpCode::get_local, result_slot); builder_.emit(bytecode::OpCode::iterator_complete);
    const auto body_jump = builder_.emit_jump(bytecode::OpCode::jump_if_false);
    const auto end_jump = builder_.emit_jump(bytecode::OpCode::jump);
    builder_.patch_jump(body_jump, static_cast<std::uint32_t>(builder_.offset()));
    builder_.emit_local(bytecode::OpCode::get_local, result_slot); builder_.emit(bytecode::OpCode::iterator_value);
    builder_.emit_local(bytecode::OpCode::set_local, value_slot); builder_.emit(bytecode::OpCode::pop);
    const auto assigned = compile_iteration_binding(*statement.left, value_slot); if (!assigned) { cleanup(); return assigned.error(); }
    controls_.push_back(ControlContext{protected_finally_depth_, true, labels, {}, {}});
    const auto body = compile_statement(*statement.body, false);
    ControlContext control = std::move(controls_.back()); controls_.pop_back();
    if (!body) { cleanup(); return body.error(); }
    for (const auto operand : control.continue_jumps) builder_.patch_jump(operand, loop_start);
    const auto repeat = builder_.emit_jump(bytecode::OpCode::jump); builder_.patch_jump(repeat, loop_start);
    const auto end = static_cast<std::uint32_t>(builder_.offset()); builder_.patch_jump(end_jump, end);
    for (const auto operand : control.break_jumps) builder_.patch_jump(operand, end);
    cleanup();
    return {};
}

Result<void> Compiler::compile_switch(const frontend::SwitchStatementNode& statement, const std::vector<std::string>& labels) {
    const auto discriminant = compile_expression(*statement.discriminant); if (!discriminant) return discriminant.error();
    const auto discriminant_slot = scopes_->allocate_temporary();
    builder_.emit_local(bytecode::OpCode::set_local, discriminant_slot); builder_.emit(bytecode::OpCode::pop);
    const auto switch_scope = scopes_->begin_switch(statement); if (!switch_scope) return switch_scope.error();
    auto cleanup = [&] { scopes_->end_block(); };
    std::vector<std::optional<std::size_t>> match_jumps(statement.cases.size());
    std::optional<std::size_t> default_index;
    for (std::size_t i = 0; i < statement.cases.size(); ++i) {
        const auto& clause = statement.cases[i];
        if (!clause.test) { default_index = i; continue; }
        builder_.emit_local(bytecode::OpCode::get_local, discriminant_slot);
        const auto test = compile_expression(*clause.test); if (!test) { cleanup(); return test.error(); }
        builder_.emit(bytecode::OpCode::strict_equal);
        const auto no_match = builder_.emit_jump(bytecode::OpCode::jump_if_false);
        match_jumps[i] = builder_.emit_jump(bytecode::OpCode::jump);
        builder_.patch_jump(no_match, static_cast<std::uint32_t>(builder_.offset()));
    }
    const auto no_match_jump = builder_.emit_jump(bytecode::OpCode::jump);
    controls_.push_back(ControlContext{protected_finally_depth_, false, labels, {}, {}});
    std::vector<std::uint32_t> clause_offsets(statement.cases.size());
    for (std::size_t i = 0; i < statement.cases.size(); ++i) {
        clause_offsets[i] = static_cast<std::uint32_t>(builder_.offset());
        if (match_jumps[i]) builder_.patch_jump(*match_jumps[i], clause_offsets[i]);
        const auto body = compile_statement_list(statement.cases[i].consequent, false, false); if (!body) { controls_.pop_back(); cleanup(); return body.error(); }
    }
    ControlContext control = std::move(controls_.back()); controls_.pop_back();
    const auto end = static_cast<std::uint32_t>(builder_.offset());
    builder_.patch_jump(no_match_jump, default_index ? clause_offsets[*default_index] : end);
    for (const auto operand : control.break_jumps) builder_.patch_jump(operand, end);
    cleanup();
    return {};
}

Result<void> Compiler::compile_labeled(const frontend::LabeledStatementNode& statement) {
    std::vector<std::string> labels;
    const frontend::ASTNode* body = &statement;
    while (body->type == frontend::ASTNodeType::LABELED_STATEMENT) {
        const auto& labeled = static_cast<const frontend::LabeledStatementNode&>(*body);
        labels.push_back(labeled.label);
        body = labeled.body.get();
    }
    switch (body->type) {
    case frontend::ASTNodeType::WHILE_STATEMENT: return compile_while(static_cast<const frontend::WhileStatementNode&>(*body), labels);
    case frontend::ASTNodeType::DO_WHILE_STATEMENT: return compile_do_while(static_cast<const frontend::DoWhileStatementNode&>(*body), labels);
    case frontend::ASTNodeType::FOR_STATEMENT: return compile_for(static_cast<const frontend::ForStatementNode&>(*body), labels);
    case frontend::ASTNodeType::FOR_IN_STATEMENT: return compile_for_in(static_cast<const frontend::ForInStatementNode&>(*body), labels);
    case frontend::ASTNodeType::FOR_OF_STATEMENT: return compile_for_of(static_cast<const frontend::ForOfStatementNode&>(*body), labels);
    case frontend::ASTNodeType::SWITCH_STATEMENT: return compile_switch(static_cast<const frontend::SwitchStatementNode&>(*body), labels);
    default: break;
    }
    controls_.push_back(ControlContext{protected_finally_depth_, false, labels, {}, {}});
    const auto compiled = compile_statement(*body, false);
    ControlContext control = std::move(controls_.back()); controls_.pop_back();
    if (!compiled) return compiled.error();
    const auto end = static_cast<std::uint32_t>(builder_.offset());
    for (const auto operand : control.break_jumps) builder_.patch_jump(operand, end);
    return {};
}

Result<void> Compiler::compile_break(const frontend::BreakStatementNode& statement) {
    auto target = controls_.rend();
    for (auto it = controls_.rbegin(); it != controls_.rend(); ++it) {
        if (!statement.label || std::find(it->labels.begin(), it->labels.end(), *statement.label) != it->labels.end()) { target = it; break; }
    }
    if (target == controls_.rend()) return error_at(statement, "break target is not active");
    const auto opcode = protected_finally_depth_ > target->protected_finally_depth ? bytecode::OpCode::break_ : bytecode::OpCode::jump;
    target->break_jumps.push_back(builder_.emit_jump(opcode));
    return {};
}

Result<void> Compiler::compile_continue(const frontend::ContinueStatementNode& statement) {
    auto target = controls_.rend();
    for (auto it = controls_.rbegin(); it != controls_.rend(); ++it) {
        if (!it->iteration) continue;
        if (!statement.label || std::find(it->labels.begin(), it->labels.end(), *statement.label) != it->labels.end()) { target = it; break; }
    }
    if (target == controls_.rend()) return error_at(statement, "continue target is not an active iteration statement");
    const auto opcode = protected_finally_depth_ > target->protected_finally_depth ? bytecode::OpCode::continue_ : bytecode::OpCode::jump;
    target->continue_jumps.push_back(builder_.emit_jump(opcode));
    return {};
}

Result<void> Compiler::compile_block(const frontend::BlockStatementNode& block) {
    const auto begin = scopes_->begin_block(block); if (!begin) return begin.error();
    const auto body = compile_statement_list(block.body, false, false);
    scopes_->end_block();
    return body;
}

Result<void> Compiler::compile_statement(const frontend::ASTNode& node, bool preserve_expression_value) {
    switch (node.type) {
    case frontend::ASTNodeType::VARIABLE_DECLARATION: return compile_variable_declaration(static_cast<const frontend::VariableDeclarationNode&>(node));
    case frontend::ASTNodeType::FUNCTION_DECLARATION: return {};
    case frontend::ASTNodeType::CLASS_DECLARATION: return compile_class_declaration(static_cast<const frontend::ClassDeclarationNode&>(node));
    case frontend::ASTNodeType::RETURN_STATEMENT: return compile_return(static_cast<const frontend::ReturnStatementNode&>(node));
    case frontend::ASTNodeType::THROW_STATEMENT: return compile_throw(static_cast<const frontend::ThrowStatementNode&>(node));
    case frontend::ASTNodeType::TRY_STATEMENT: return compile_try(static_cast<const frontend::TryStatementNode&>(node));
    case frontend::ASTNodeType::IF_STATEMENT: return compile_if(static_cast<const frontend::IfStatementNode&>(node));
    case frontend::ASTNodeType::WITH_STATEMENT: return error_at(node, "with statement requires dynamic-environment entry and compiler Reference lowering");
    case frontend::ASTNodeType::WHILE_STATEMENT: return compile_while(static_cast<const frontend::WhileStatementNode&>(node));
    case frontend::ASTNodeType::DO_WHILE_STATEMENT: return compile_do_while(static_cast<const frontend::DoWhileStatementNode&>(node));
    case frontend::ASTNodeType::FOR_STATEMENT: return compile_for(static_cast<const frontend::ForStatementNode&>(node));
    case frontend::ASTNodeType::FOR_IN_STATEMENT: return compile_for_in(static_cast<const frontend::ForInStatementNode&>(node));
    case frontend::ASTNodeType::FOR_OF_STATEMENT: return compile_for_of(static_cast<const frontend::ForOfStatementNode&>(node));
    case frontend::ASTNodeType::SWITCH_STATEMENT: return compile_switch(static_cast<const frontend::SwitchStatementNode&>(node));
    case frontend::ASTNodeType::LABELED_STATEMENT: return compile_labeled(static_cast<const frontend::LabeledStatementNode&>(node));
    case frontend::ASTNodeType::BREAK_STATEMENT: return compile_break(static_cast<const frontend::BreakStatementNode&>(node));
    case frontend::ASTNodeType::CONTINUE_STATEMENT: return compile_continue(static_cast<const frontend::ContinueStatementNode&>(node));
    case frontend::ASTNodeType::DEBUGGER_STATEMENT: return {};
    case frontend::ASTNodeType::BLOCK_STATEMENT: return compile_block(static_cast<const frontend::BlockStatementNode&>(node));
    case frontend::ASTNodeType::EXPRESSION_STATEMENT: {
        const auto& statement = static_cast<const frontend::ExpressionStatementNode&>(node);
        const auto expression = compile_expression(*statement.expression); if (!expression) return expression.error();
        if (!preserve_expression_value) builder_.emit(bytecode::OpCode::pop);
        return {};
    }
    case frontend::ASTNodeType::IMPORT_DECLARATION:
        if (!module_mode_) return error_at(node, "import declarations require module compilation");
        return {};
    case frontend::ASTNodeType::EXPORT_NAMED_DECLARATION: {
        if (!module_mode_) return error_at(node, "export declarations require module compilation");
        const auto& exported = static_cast<const frontend::ExportNamedDeclarationNode&>(node);
        if (exported.declaration) return compile_statement(*exported.declaration, false);
        return {};
    }
    case frontend::ASTNodeType::EMPTY_STATEMENT: return {};
    default: return error_at(node, "statement node is parsed but not executable");
    }
}

Result<void> Compiler::compile_statement_list(const std::vector<std::unique_ptr<frontend::ASTNode>>& statements, bool allow_final_expression_value, bool hoist_functions) {
    if (hoist_functions) {
        for (const auto& statement : statements) {
            const frontend::ASTNode* hoisted_node = statement.get();
            if (statement->type == frontend::ASTNodeType::EXPORT_NAMED_DECLARATION) {
                const auto& exported = static_cast<const frontend::ExportNamedDeclarationNode&>(*statement);
                hoisted_node = exported.declaration.get();
            }
            if (hoisted_node != nullptr && hoisted_node->type == frontend::ASTNodeType::FUNCTION_DECLARATION) {
                const auto hoisted = compile_function_declaration(static_cast<const frontend::FunctionDeclarationNode&>(*hoisted_node));
                if (!hoisted) return hoisted.error();
            }
        }
    }
    for (std::size_t index = 0; index < statements.size(); ++index) {
        const auto& statement = *statements[index];
        const bool is_last = index + 1U == statements.size();
        const bool preserve = allow_final_expression_value && is_last && statement.type == frontend::ASTNodeType::EXPRESSION_STATEMENT;
        const auto result = compile_statement(statement, preserve); if (!result) return result.error();
    }
    return {};
}

Result<bytecode::BytecodeChunk> Compiler::compile(const frontend::ProgramNode& program) {
    if (used_) return Error{ErrorCode::compile_error, "Compiler instances are single-use"};
    used_ = true;
    strict_ = has_use_strict_directive(program.body);
    const auto scope_setup = scopes_->begin_program(program); if (!scope_setup) return scope_setup.error();
    const bool final_expression_preserved = !program.body.empty() && program.body.back()->type == frontend::ASTNodeType::EXPRESSION_STATEMENT;
    const auto body = compile_statement_list(program.body, true, true); if (!body) return body.error();
    if (!final_expression_preserved) builder_.emit(bytecode::OpCode::undefined);
    builder_.emit(bytecode::OpCode::return_);
    builder_.set_local_count(scopes_->local_count());
    builder_.set_local_binding_states(scopes_->local_binding_states());
    builder_.set_local_binding_immutable(scopes_->local_binding_immutable());
    return std::move(builder_).finish();
}

Result<bytecode::BytecodeChunk> compile_program(Context& context, const frontend::ProgramNode& program) {
    Compiler compiler(context);
    return compiler.compile(program);
}

Result<bytecode::BytecodeChunk> compile_module(
    Context& context,
    const frontend::ProgramNode& program,
    const std::vector<ModuleImportBinding>& imports,
    const std::vector<ModuleExportRequest>& exports) {
    std::unordered_map<std::string, std::uint32_t> import_map;
    for (const auto& entry : imports) {
        if (!import_map.emplace(entry.name, entry.module_index).second) {
            return Error{ErrorCode::compile_error, "duplicate module import binding '" + entry.name + "'"};
        }
    }
    const std::size_t binding_count = imports.size() + exports.size();
    if (binding_count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return Error{ErrorCode::compile_error, "module binding count exceeds 32-bit index space"};
    }

    Compiler compiler(context, &import_map, static_cast<std::uint32_t>(binding_count));
    compiler.used_ = true;
    const auto setup = compiler.scopes_->begin_program(program);
    if (!setup) return setup.error();

    std::vector<bytecode::ModuleExportBinding> export_bindings;
    export_bindings.reserve(exports.size());
    for (const auto& request : exports) {
        auto* binding = compiler.scopes_->find_binding(request.local_name);
        if (binding == nullptr || binding->kind == detail::BindingKind::import_binding) {
            return Error{ErrorCode::compile_error, "export '" + request.export_name + "' does not name a local binding"};
        }
        export_bindings.push_back(bytecode::ModuleExportBinding{binding->slot, request.module_index});
    }

    const auto body = compiler.compile_statement_list(program.body, false, true);
    if (!body) return body.error();
    compiler.builder_.emit(bytecode::OpCode::undefined);
    compiler.builder_.emit(bytecode::OpCode::return_);
    compiler.builder_.set_local_count(compiler.scopes_->local_count());
    compiler.builder_.set_local_binding_states(compiler.scopes_->local_binding_states());
    compiler.builder_.set_local_binding_immutable(compiler.scopes_->local_binding_immutable());
    compiler.builder_.set_module_binding_count(static_cast<std::uint32_t>(binding_count));
    compiler.builder_.set_module_exports(std::move(export_bindings));
    return std::move(compiler.builder_).finish();
}

} // namespace js::compiler
