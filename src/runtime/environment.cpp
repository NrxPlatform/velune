#include <js/environment.hpp>

#include <js/context.hpp>
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

void ObjectEnvironmentRecord::attach_binding_object(Context& context, Value binding_object) noexcept {
    context_ = &context;
    binding_object_ = binding_object;
}

bool ObjectEnvironmentRecord::has_object_binding(std::string_view name) const {
    if (context_ == nullptr || !binding_object_.is_object_like()) return false;
    const auto present = context_->has_property(binding_object_, name);
    return present && *present;
}

ExecutionResult ObjectEnvironmentRecord::get_object_binding_value(std::string_view name) {
    if (context_ == nullptr || !binding_object_.is_object_like())
        return EngineFailure{EngineFailureCode::InternalInvariant, "object environment has no binding object"};
    if (!has_object_binding(name))
        return Completion::throw_(context_->reference_error("binding '" + std::string(name) + "' is not defined"));
    return context_->get_property_semantic(binding_object_, context_->property_key(name), binding_object_);
}

ExecutionResult ObjectEnvironmentRecord::set_object_binding_value(std::string_view name, Value value, bool strict) {
    if (context_ == nullptr || !binding_object_.is_object_like())
        return EngineFailure{EngineFailureCode::InternalInvariant, "object environment has no binding object"};

    const ExecutionResult set = context_->set_property_semantic(
        binding_object_, context_->property_key(name), value, binding_object_);
    if (!set) return set.error();
    if (!set.completion().is_normal()) return set.completion();
    if (!set.completion().value().is_boolean())
        return EngineFailure{EngineFailureCode::InternalInvariant, "object environment [[Set]] did not return boolean"};
    if (!set.completion().value().as_boolean() && strict)
        return Completion::throw_(context_->type_error("assignment to non-writable global property '" + std::string(name) + "'"));

    const auto still_exists = context_->has_property(binding_object_, name);
    if (!still_exists) return still_exists.error();
    if (!*still_exists && strict)
        return Completion::throw_(context_->reference_error("binding '" + std::string(name) + "' is not defined"));
    return Completion::normal(Value::undefined());
}

Result<void> ObjectEnvironmentRecord::create_object_binding(
    std::string_view name, Value value, bool writable, bool enumerable, bool configurable) {
    if (context_ == nullptr || !binding_object_.is_object_like())
        return Error{ErrorCode::internal, "object environment has no binding object"};
    const auto defined = context_->define_own_property(
        binding_object_, context_->property_key(name),
        PropertyDescriptor::data(value, writable, enumerable, configurable));
    if (!defined) return defined.error();
    if (!*defined) return Error{ErrorCode::type_error, "cannot create global property '" + std::string(name) + "'"};
    return {};
}

std::vector<Value> ObjectEnvironmentRecord::object_binding_values() const {
    // The binding object is already a GC root through Realm::global_object_.
    // Do not duplicate all of its property values as environment roots.
    return {};
}

void GlobalEnvironmentRecord::attach_global_object(Context& context, Value global_object) noexcept {
    context_ = &context;
    object_record_.attach_binding_object(context, global_object);
}

bool GlobalEnvironmentRecord::has_binding(std::string_view name) const {
    return lexical_record_.has_binding(name) || object_record_.has_object_binding(name);
}

Result<void> GlobalEnvironmentRecord::create_global_var_binding(std::string name, Value value, bool configurable) {
    if (lexical_record_.has_binding(name))
        return Error{ErrorCode::compile_error, "global var conflicts with lexical binding '" + name + "'"};
    if (!object_record_.has_object_binding(name))
        return object_record_.create_object_binding(name, value, true, true, configurable);
    return {};
}

Result<void> GlobalEnvironmentRecord::create_global_constant_binding(std::string name, Value value) {
    if (lexical_record_.has_binding(name))
        return Error{ErrorCode::compile_error, "global constant conflicts with lexical binding '" + name + "'"};
    if (object_record_.has_object_binding(name))
        return Error{ErrorCode::internal, "global constant binding '" + name + "' already exists"};
    return object_record_.create_object_binding(name, value, false, false, false);
}

Result<void> GlobalEnvironmentRecord::create_global_lexical_binding(std::string name, bool immutable) {
    if (has_binding(name)) return Error{ErrorCode::compile_error, "duplicate global lexical binding '" + name + "'"};
    return immutable ? lexical_record_.create_immutable_binding(std::move(name), false)
                     : lexical_record_.create_mutable_binding(std::move(name), false);
}

Result<void> GlobalEnvironmentRecord::initialize_lexical_binding(std::string_view name, Value value) {
    return lexical_record_.initialize_binding(name, value);
}

ExecutionResult GlobalEnvironmentRecord::set_mutable_binding(std::string_view name, Value value, bool strict) {
    if (lexical_record_.has_binding(name)) {
        const auto set = lexical_record_.set_mutable_binding(name, value);
        if (set) return Completion::normal(Value::undefined());
        if (context_ != nullptr && set.error().code() == ErrorCode::reference_error)
            return Completion::throw_(context_->reference_error(set.error().message()));
        if (context_ != nullptr && set.error().code() == ErrorCode::type_error)
            return Completion::throw_(context_->type_error(set.error().message()));
        return set.error();
    }
    return object_record_.set_object_binding_value(name, value, strict);
}

ExecutionResult GlobalEnvironmentRecord::get_binding_value(std::string_view name) {
    if (lexical_record_.has_binding(name)) {
        const auto value = lexical_record_.get_binding_value(name);
        if (value) return Completion::normal(*value);
        if (context_ != nullptr && value.error().code() == ErrorCode::reference_error)
            return Completion::throw_(context_->reference_error(value.error().message()));
        return value.error();
    }
    return object_record_.get_object_binding_value(name);
}

std::vector<Value> GlobalEnvironmentRecord::binding_values() const {
    return lexical_record_.binding_values();
}

} // namespace js
