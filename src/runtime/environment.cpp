#include <js/environment.hpp>

#include <js/error.hpp>

namespace js {

bool EnvironmentRecord::has_binding(std::string_view name) const noexcept {
    return bindings_.find(std::string(name)) != bindings_.end();
}

Result<void> EnvironmentRecord::create_mutable_binding(std::string name, bool initialized) {
    if (bindings_.contains(name)) return Error{ErrorCode::internal, "binding '" + name + "' already exists"};
    NamedBinding binding;
    binding.slot.state = initialized ? BindingState::InitializedMutable : BindingState::Uninitialized;
    bindings_.emplace(std::move(name), binding);
    return {};
}

Result<void> EnvironmentRecord::create_immutable_binding(std::string name, bool initialized) {
    if (bindings_.contains(name)) return Error{ErrorCode::internal, "binding '" + name + "' already exists"};
    NamedBinding binding;
    binding.slot.state = initialized ? BindingState::InitializedImmutable : BindingState::Uninitialized;
    binding.immutable = true;
    bindings_.emplace(std::move(name), binding);
    return {};
}

Result<void> EnvironmentRecord::initialize_binding(std::string_view name, Value value) {
    auto found = bindings_.find(std::string(name));
    if (found == bindings_.end()) return Error{ErrorCode::reference_error, "binding '" + std::string(name) + "' is not defined"};
    if (found->second.slot.initialized()) return Error{ErrorCode::internal, "binding '" + std::string(name) + "' is already initialized"};
    found->second.slot.value = value;
    found->second.slot.state = found->second.immutable ? BindingState::InitializedImmutable : BindingState::InitializedMutable;
    return {};
}

Result<void> EnvironmentRecord::set_mutable_binding(std::string_view name, Value value) {
    auto found = bindings_.find(std::string(name));
    if (found == bindings_.end()) return Error{ErrorCode::reference_error, "binding '" + std::string(name) + "' is not defined"};
    if (!found->second.slot.initialized()) return Error{ErrorCode::reference_error, "cannot access '" + std::string(name) + "' before initialization"};
    if (!found->second.slot.mutable_binding()) return Error{ErrorCode::type_error, "assignment to immutable binding '" + std::string(name) + "'"};
    found->second.slot.value = value;
    return {};
}

Result<Value> EnvironmentRecord::get_binding_value(std::string_view name) const {
    const auto found = bindings_.find(std::string(name));
    if (found == bindings_.end()) return Error{ErrorCode::reference_error, "binding '" + std::string(name) + "' is not defined"};
    if (!found->second.slot.initialized()) return Error{ErrorCode::reference_error, "cannot access '" + std::string(name) + "' before initialization"};
    return found->second.slot.value;
}

Result<bool> EnvironmentRecord::delete_binding(std::string_view name) {
    const auto found = bindings_.find(std::string(name));
    if (found == bindings_.end()) return true;
    if (!found->second.deletable) return false;
    bindings_.erase(found);
    return true;
}

std::vector<Value> EnvironmentRecord::binding_values() const {
    std::vector<Value> values;
    values.reserve(bindings_.size());
    for (const auto& [name, binding] : bindings_) {
        (void)name;
        if (binding.slot.initialized()) values.push_back(binding.slot.value);
    }
    return values;
}

bool GlobalEnvironmentRecord::has_binding(std::string_view name) const noexcept {
    return lexical_record_.has_binding(name) || object_record_.has_binding(name);
}

Result<void> GlobalEnvironmentRecord::create_global_var_binding(std::string name, Value value) {
    if (lexical_record_.has_binding(name))
        return Error{ErrorCode::compile_error, "global var conflicts with lexical binding '" + name + "'"};
    if (!object_record_.has_binding(name)) {
        const auto created = object_record_.create_mutable_binding(name, false); if (!created) return created.error();
        return object_record_.initialize_binding(name, value);
    }
    return object_record_.set_mutable_binding(name, value);
}

Result<void> GlobalEnvironmentRecord::create_global_constant_binding(std::string name, Value value) {
    if (lexical_record_.has_binding(name))
        return Error{ErrorCode::compile_error, "global constant conflicts with lexical binding '" + name + "'"};
    if (object_record_.has_binding(name))
        return Error{ErrorCode::internal, "global constant binding '" + name + "' already exists"};
    const auto created = object_record_.create_immutable_binding(name, false);
    if (!created) return created.error();
    return object_record_.initialize_binding(name, value);
}

Result<void> GlobalEnvironmentRecord::create_global_lexical_binding(std::string name, bool immutable) {
    if (has_binding(name)) return Error{ErrorCode::compile_error, "duplicate global lexical binding '" + name + "'"};
    return immutable ? lexical_record_.create_immutable_binding(std::move(name), false)
                     : lexical_record_.create_mutable_binding(std::move(name), false);
}

Result<void> GlobalEnvironmentRecord::initialize_lexical_binding(std::string_view name, Value value) {
    return lexical_record_.initialize_binding(name, value);
}

Result<void> GlobalEnvironmentRecord::set_mutable_binding(std::string_view name, Value value) {
    if (lexical_record_.has_binding(name)) return lexical_record_.set_mutable_binding(name, value);
    return object_record_.set_mutable_binding(name, value);
}

Result<Value> GlobalEnvironmentRecord::get_binding_value(std::string_view name) const {
    if (lexical_record_.has_binding(name)) return lexical_record_.get_binding_value(name);
    return object_record_.get_binding_value(name);
}

std::vector<Value> GlobalEnvironmentRecord::binding_values() const {
    auto values = lexical_record_.binding_values();
    auto object_values = object_record_.binding_values();
    values.insert(values.end(), object_values.begin(), object_values.end());
    return values;
}

} // namespace js
