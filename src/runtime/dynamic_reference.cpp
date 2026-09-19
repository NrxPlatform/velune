#include <js/dynamic_reference.hpp>

#include <js/abstract_operations.hpp>
#include <js/context.hpp>
#include <js/environment.hpp>

#include "detail/heap.hpp"

namespace js {
namespace {
ExecutionResult has_with_binding(Context& context, Value object, std::string_view name) {
    const auto present = context.has_property(object, name);
    if (!present) return present.error();
    if (!*present) return Completion::normal(Value::boolean(false));

    const auto unscopables_key = context.property_key(context.well_known_symbol("unscopables"));
    if (!unscopables_key) return unscopables_key.error();
    const auto exclusions = context.get_property_semantic(object, *unscopables_key, object);
    if (!exclusions) return exclusions.error();
    if (!exclusions.completion().is_normal()) return exclusions.completion();
    const Value table = exclusions.completion().value();
    if (!table.is_object_like()) return Completion::normal(Value::boolean(true));
    const auto excluded = context.get_property_semantic(table, context.property_key(name), table);
    if (!excluded) return excluded.error();
    if (!excluded.completion().is_normal()) return excluded.completion();
    return Completion::normal(Value::boolean(!abstract_operations::to_boolean(excluded.completion().value())));
}
} // namespace

ExecutionResult resolve_dynamic_binding(Context& context, detail::HeapDynamicEnvironment* active,
                                        GlobalEnvironmentRecord& global, std::string_view name,
                                        bool strict, DynamicBindingReference& output,
                                        detail::HeapUpvalue* static_fallback) {
    // Do not publish a partially resolved Reference if a property trap throws.
    DynamicBindingReference candidate;
    candidate.name = std::string(name);
    candidate.strict = strict;
    candidate.global = &global;
    for (auto* current = active; current != nullptr; current = current->outer) {
        const auto found = has_with_binding(context, current->binding_object, name);
        if (!found) return found.error();
        if (!found.completion().is_normal()) return found.completion();
        if (found.completion().value().as_boolean()) {
            candidate.target = DynamicBindingReference::Target::ObjectEnvironment;
            candidate.environment = current;
            output = std::move(candidate);
            return Completion::normal();
        }
    }
    if (static_fallback != nullptr) {
        candidate.target = DynamicBindingReference::Target::StaticBinding;
        candidate.static_binding = static_fallback;
        output = std::move(candidate);
        return Completion::normal();
    }
    candidate.target = global.has_binding(name)
        ? DynamicBindingReference::Target::GlobalEnvironment
        : DynamicBindingReference::Target::Unresolvable;
    output = std::move(candidate);
    return Completion::normal();
}

ExecutionResult DynamicBindingReference::get(Context& context) const {
    if (target == Target::Unresolvable)
        return Completion::throw_(context.reference_error("binding '" + name + "' is not defined"));
    if (target == Target::StaticBinding) {
        if (static_binding == nullptr) return EngineFailure{EngineFailureCode::InternalInvariant, "missing static fallback"};
        if (static_binding->state() == BindingState::Uninitialized)
            return Completion::throw_(context.reference_error("cannot access lexical binding before initialization"));
        return Completion::normal(static_binding->get());
    }
    if (target == Target::GlobalEnvironment) return global->get_binding_value(name);
    const Value object = environment->binding_object;
    // A selected object environment is retained even if its property vanished.
    if (!object.is_object_like()) return EngineFailure{EngineFailureCode::InternalInvariant, "invalid with binding object"};
    const auto present = context.has_property(object, name);
    if (!present) return present.error();
    if (!*present) return Completion::throw_(context.reference_error("binding '" + name + "' is not defined"));
    return context.get_property_semantic(object, context.property_key(name), object);
}

ExecutionResult DynamicBindingReference::put(Context& context, Value value) const {
    if (target == Target::Unresolvable) {
        if (strict) return Completion::throw_(context.reference_error("binding '" + name + "' is not defined"));
        return global->set_mutable_binding(name, value, false);
    }
    if (target == Target::StaticBinding) {
        if (static_binding == nullptr) return EngineFailure{EngineFailureCode::InternalInvariant, "missing static fallback"};
        if (static_binding->state() == BindingState::Uninitialized)
            return Completion::throw_(context.reference_error("cannot assign to lexical binding before initialization"));
        if (static_binding->immutable())
            return Completion::throw_(context.type_error("assignment to immutable binding"));
        static_binding->set(value);
        return Completion::normal();
    }
    if (target == Target::GlobalEnvironment) return global->set_mutable_binding(name, value, strict);
    const Value object = environment->binding_object;
    const auto set = context.set_property_semantic(object, context.property_key(name), value, object);
    if (!set) return set.error();
    if (!set.completion().is_normal()) return set.completion();
    if (!set.completion().value().is_boolean())
        return EngineFailure{EngineFailureCode::InternalInvariant, "object environment [[Set]] did not return boolean"};
    if (!set.completion().value().as_boolean() && strict)
        return Completion::throw_(context.type_error("assignment to non-writable property '" + name + "'"));
    const auto present = context.has_property(object, name);
    if (!present) return present.error();
    if (!*present && strict)
        return Completion::throw_(context.reference_error("binding '" + name + "' is not defined"));
    return Completion::normal();
}

ExecutionResult DynamicBindingReference::delete_binding(Context& context) const {
    if (target == Target::Unresolvable) return Completion::normal(Value::boolean(true));
    if (target == Target::StaticBinding) return Completion::normal(Value::boolean(false));
    if (target == Target::GlobalEnvironment)
        return global->delete_binding_semantic(name);
    const auto deleted = context.delete_property_semantic(environment->binding_object, context.property_key(name));
    if (!deleted) return deleted.error();
    if (!deleted.completion().is_normal()) return deleted.completion();
    if (strict && !deleted.completion().value().as_boolean())
        return Completion::throw_(context.type_error("cannot delete binding '" + name + "'"));
    return deleted.completion();
}
} // namespace js
