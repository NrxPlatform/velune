#include <js/context.hpp>
#include <js/abstract_operations.hpp>
#include <js/vm/vm.hpp>

#include <string>
#include <algorithm>
#include <charconv>
#include <optional>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "detail/heap.hpp"

namespace js {
namespace detail {
struct ValueAccess final {
    static HeapObject* object(Value value) { return value.as_heap_object(); }
    static const HeapFunction* function(Value value) { return value.as_heap_function(); }
    static HeapFunction* mutable_function(Value value) { return const_cast<HeapFunction*>(value.as_heap_function()); }
};
}
namespace {
Value argument_or_undefined(std::span<const Value> arguments, std::size_t index) noexcept {
    return index < arguments.size() ? arguments[index] : Value::undefined();
}

Value make_error_instance(Context& c, Value prototype, std::string_view name, std::string_view message) {
    auto object = c.object(prototype);
    if (!object) return Value::undefined();
    (void)c.set_own_property(*object, "name", c.string(name));
    (void)c.set_own_property(*object, "message", c.string(message));
    return *object;
}

ExecutionResult builtin_error_constructor(Context& c, Value, std::span<const Value> a) {
    const Value message = argument_or_undefined(a, 0);
    return Completion::normal(c.error(message.is_undefined() ? std::string_view{} : message.to_debug_string()));
}
ExecutionResult builtin_type_error_constructor(Context& c, Value, std::span<const Value> a) {
    const Value message = argument_or_undefined(a, 0);
    return Completion::normal(c.type_error(message.is_undefined() ? std::string_view{} : message.to_debug_string()));
}
ExecutionResult builtin_reference_error_constructor(Context& c, Value, std::span<const Value> a) {
    const Value message = argument_or_undefined(a, 0);
    return Completion::normal(c.reference_error(message.is_undefined() ? std::string_view{} : message.to_debug_string()));
}

ExecutionResult builtin_function_prototype(Context&, Value, std::span<const Value>) {
    return Completion::normal(Value::undefined());
}

ExecutionResult builtin_function_constructor(Context& c, Value, std::span<const Value>) {
    return Completion::throw_(c.type_error("dynamic Function construction is not implemented"));
}

ExecutionResult builtin_push(Context& c, Value t, std::span<const Value> a) { return execution_from_result(c.array_push(t, argument_or_undefined(a, 0))); }
ExecutionResult builtin_pop(Context& c, Value t, std::span<const Value>) { return execution_from_result(c.array_pop(t)); }
ExecutionResult builtin_array_iterator(Context& c, Value t, std::span<const Value>) { return execution_from_result(c.array_iterator(t)); }
ExecutionResult builtin_array_iterator_next(Context& c, Value t, std::span<const Value>) { return execution_from_result(c.array_iterator_next(t)); }
ExecutionResult builtin_is_array(Context& c, Value, std::span<const Value> a) { return Completion::normal(c.boolean(c.is_array(argument_or_undefined(a, 0)))); }
ExecutionResult builtin_array_constructor(Context& c, Value, std::span<const Value> arguments) {
    Value array = c.array();
    if (arguments.empty()) return Completion::normal(array);

    if (arguments.size() == 1U && arguments[0].is_number()) {
        const double length = arguments[0].as_number();
        const double integral = std::floor(length);
        if (!std::isfinite(length) || length < 0.0 || length > 4294967295.0 || integral != length)
            return Error{ErrorCode::type_error, "invalid array length"};
        PropertyDescriptor descriptor;
        descriptor.value = Value::number(length);
        const auto defined = c.define_own_property(array, c.property_key("length"), descriptor);
        if (!defined) return defined.error();
        if (!*defined) return Error{ErrorCode::type_error, "failed to define Array length"};
        return Completion::normal(array);
    }

    for (Value value : arguments) {
        const auto pushed = c.array_push(array, value);
        if (!pushed) return pushed.error();
    }
    return Completion::normal(array);
}
ExecutionResult builtin_string_constructor(Context& c, Value, std::span<const Value> a) {
    if (a.empty()) return Completion::normal(c.string(""));
    const auto result = abstract_operations::to_string(c, a[0]);
    if (!result) return result.error();
    return result.completion();
}

ExecutionResult builtin_is_finite(Context& c, Value, std::span<const Value> a) { const Value v = argument_or_undefined(a, 0); return Completion::normal(c.boolean(v.is_number() && std::isfinite(v.as_number()))); }
ExecutionResult builtin_get_proto(Context& c, Value, std::span<const Value> a) { return execution_from_result(c.get_prototype(argument_or_undefined(a, 0))); }

ExecutionResult builtin_object_define_property(Context& c, Value, std::span<const Value> a) {
    const Value target = argument_or_undefined(a, 0);
    if (!target.is_object_like()) return Error{ErrorCode::type_error, "Object.defineProperty target is not an object"};

    const auto key_result = abstract_operations::to_property_key(c, argument_or_undefined(a, 1));
    if (!key_result) return key_result.error();
    if (!key_result.completion().is_normal()) return key_result.completion();
    const Value key_value = key_result.completion().value();
    const PropertyKey key = key_value.is_symbol()
        ? PropertyKey::symbol(key_value.as_symbol_id())
        : c.property_key(key_value.as_string());

    const Value attributes = argument_or_undefined(a, 2);
    if (!attributes.is_object_like()) return Error{ErrorCode::type_error, "Object.defineProperty descriptor is not an object"};

    PropertyDescriptor descriptor;
    auto read_descriptor_field = [&](std::string_view name) -> ExecutionResult {
        const PropertyKey field_key = c.property_key(name);
        const auto present = c.has_property(attributes, field_key);
        if (!present) return present.error();
        if (!*present) return Completion::normal(Value::undefined());
        return c.get_property_semantic(attributes, field_key);
    };

    const auto enumerable = read_descriptor_field("enumerable");
    if (!enumerable) return enumerable.error();
    if (!enumerable.completion().is_normal()) return enumerable.completion();
    const auto has_enumerable = c.has_property(attributes, c.property_key("enumerable"));
    if (!has_enumerable) return has_enumerable.error();
    if (*has_enumerable)
        descriptor.enumerable = abstract_operations::to_boolean(enumerable.completion().value());

    const auto configurable = read_descriptor_field("configurable");
    if (!configurable) return configurable.error();
    if (!configurable.completion().is_normal()) return configurable.completion();
    const auto has_configurable = c.has_property(attributes, c.property_key("configurable"));
    if (!has_configurable) return has_configurable.error();
    if (*has_configurable)
        descriptor.configurable = abstract_operations::to_boolean(configurable.completion().value());

    const auto value = read_descriptor_field("value");
    if (!value) return value.error();
    if (!value.completion().is_normal()) return value.completion();
    const auto has_value = c.has_property(attributes, c.property_key("value"));
    if (!has_value) return has_value.error();
    if (*has_value) descriptor.value = value.completion().value();

    const auto writable = read_descriptor_field("writable");
    if (!writable) return writable.error();
    if (!writable.completion().is_normal()) return writable.completion();
    const auto has_writable = c.has_property(attributes, c.property_key("writable"));
    if (!has_writable) return has_writable.error();
    if (*has_writable) descriptor.writable = abstract_operations::to_boolean(writable.completion().value());

    const auto getter = read_descriptor_field("get");
    if (!getter) return getter.error();
    if (!getter.completion().is_normal()) return getter.completion();
    const auto has_getter = c.has_property(attributes, c.property_key("get"));
    if (!has_getter) return has_getter.error();
    if (*has_getter) {
        const Value getter_value = getter.completion().value();
        if (!getter_value.is_undefined() && !abstract_operations::is_callable(getter_value))
            return Error{ErrorCode::type_error, "Object.defineProperty getter is not callable"};
        descriptor.get = getter_value;
    }

    const auto setter = read_descriptor_field("set");
    if (!setter) return setter.error();
    if (!setter.completion().is_normal()) return setter.completion();
    const auto has_setter = c.has_property(attributes, c.property_key("set"));
    if (!has_setter) return has_setter.error();
    if (*has_setter) {
        const Value setter_value = setter.completion().value();
        if (!setter_value.is_undefined() && !abstract_operations::is_callable(setter_value))
            return Error{ErrorCode::type_error, "Object.defineProperty setter is not callable"};
        descriptor.set = setter_value;
    }

    if (descriptor.is_accessor_descriptor() && descriptor.is_data_descriptor())
        return Error{ErrorCode::type_error, "invalid property descriptor: cannot mix accessors with value/writable"};

    const auto defined = c.define_own_property(target, key, descriptor);
    if (!defined) return defined.error();
    if (!*defined) return Error{ErrorCode::type_error, "Object.defineProperty rejected property definition"};
    return Completion::normal(target);
}

Result<Value> from_property_descriptor(Context& c, const PropertyDescriptor& descriptor) {
    Value result = c.object();
    auto define_field = [&](std::string_view name, Value value) -> Result<void> {
        const auto defined = c.define_own_property(
            result,
            name,
            PropertyDescriptor::data(value, true, true, true));
        if (!defined) return defined.error();
        if (!*defined) return Error{ErrorCode::type_error, "failed to create property descriptor object"};
        return {};
    };

    if (descriptor.is_data_descriptor()) {
        const auto value = define_field("value", descriptor.value.value_or(Value::undefined()));
        if (!value) return value.error();
        const auto writable = define_field("writable", c.boolean(descriptor.writable.value_or(false)));
        if (!writable) return writable.error();
    } else if (descriptor.is_accessor_descriptor()) {
        const auto getter = define_field("get", descriptor.get.value_or(Value::undefined()));
        if (!getter) return getter.error();
        const auto setter = define_field("set", descriptor.set.value_or(Value::undefined()));
        if (!setter) return setter.error();
    }

    const auto enumerable = define_field("enumerable", c.boolean(descriptor.enumerable.value_or(false)));
    if (!enumerable) return enumerable.error();
    const auto configurable = define_field("configurable", c.boolean(descriptor.configurable.value_or(false)));
    if (!configurable) return configurable.error();
    return result;
}

ExecutionResult builtin_object_get_own_property_descriptor(Context& c, Value, std::span<const Value> a) {
    const auto object_result = abstract_operations::to_object(c, argument_or_undefined(a, 0));
    if (!object_result) return object_result.error();
    if (!object_result.completion().is_normal()) return object_result.completion();
    const Value target = object_result.completion().value();

    const auto key_result = abstract_operations::to_property_key(c, argument_or_undefined(a, 1));
    if (!key_result) return key_result.error();
    if (!key_result.completion().is_normal()) return key_result.completion();
    const Value key_value = key_result.completion().value();
    const PropertyKey key = key_value.is_symbol()
        ? PropertyKey::symbol(key_value.as_symbol_id())
        : c.property_key(key_value.as_string());

    const auto descriptor = c.get_own_property_descriptor(target, key);
    if (!descriptor) return descriptor.error();
    if (!descriptor->has_value()) return Completion::normal(Value::undefined());
    const auto result = from_property_descriptor(c, **descriptor);
    if (!result) return result.error();
    return Completion::normal(*result);
}

ExecutionResult builtin_object_get_own_property_names(Context& c, Value, std::span<const Value> a) {
    const auto object_result = abstract_operations::to_object(c, argument_or_undefined(a, 0));
    if (!object_result) return object_result.error();
    if (!object_result.completion().is_normal()) return object_result.completion();
    const Value target = object_result.completion().value();

    const auto keys = c.own_property_keys(target);
    if (!keys) return keys.error();
    Value result = c.array();
    for (const PropertyKey key : *keys) {
        if (!key.is_atom()) continue;
        const auto pushed = c.array_push(result, c.string(std::string(c.property_key_text(key))));
        if (!pushed) return pushed.error();
    }
    return Completion::normal(result);
}

ExecutionResult builtin_object_property_is_enumerable(Context& c, Value this_value, std::span<const Value> a) {
    const auto object_result = abstract_operations::to_object(c, this_value);
    if (!object_result) return object_result.error();
    if (!object_result.completion().is_normal()) return object_result.completion();
    const Value target = object_result.completion().value();

    const auto key_result = abstract_operations::to_property_key(c, argument_or_undefined(a, 0));
    if (!key_result) return key_result.error();
    if (!key_result.completion().is_normal()) return key_result.completion();
    const Value key_value = key_result.completion().value();
    const PropertyKey key = key_value.is_symbol()
        ? PropertyKey::symbol(key_value.as_symbol_id())
        : c.property_key(key_value.as_string());

    const auto descriptor = c.get_own_property_descriptor(target, key);
    if (!descriptor) return descriptor.error();
    return Completion::normal(c.boolean(
        descriptor->has_value() && (**descriptor).enumerable.value_or(false)));
}

ExecutionResult builtin_math_pow(Context& c, Value, std::span<const Value> a) {
    const auto base_result = abstract_operations::to_number(c, argument_or_undefined(a, 0));
    if (!base_result) return base_result.error();
    if (!base_result.completion().is_normal()) return base_result.completion();
    const auto exponent_result = abstract_operations::to_number(c, argument_or_undefined(a, 1));
    if (!exponent_result) return exponent_result.error();
    if (!exponent_result.completion().is_normal()) return exponent_result.completion();
    return Completion::normal(Value::number(std::pow(
        base_result.completion().value().as_number(),
        exponent_result.completion().value().as_number())));
}

ExecutionResult builtin_object_has_own_property(Context& c, Value t, std::span<const Value> a) {
    const auto object_result = abstract_operations::to_object(c, t);
    if (!object_result) return object_result.error();
    if (!object_result.completion().is_normal()) return object_result.completion();
    const Value target = object_result.completion().value();

    const auto key_result = abstract_operations::to_property_key(c, argument_or_undefined(a, 0));
    if (!key_result) return key_result.error();
    if (!key_result.completion().is_normal()) return key_result.completion();
    const Value key_value = key_result.completion().value();
    const PropertyKey key = key_value.is_symbol()
        ? PropertyKey::symbol(key_value.as_symbol_id())
        : c.property_key(key_value.as_string());

    const auto descriptor = c.get_own_property_descriptor(target, key);
    if (!descriptor) return descriptor.error();
    return Completion::normal(c.boolean(descriptor->has_value()));
}

ExecutionResult builtin_object_is(Context& c, Value, std::span<const Value> a) {
    const auto equal = abstract_operations::same_value(
        c,
        argument_or_undefined(a, 0),
        argument_or_undefined(a, 1));
    if (!equal) return equal.error();
    return Completion::normal(c.boolean(*equal));
}

ExecutionResult builtin_object_keys(Context& c, Value, std::span<const Value> a) {
    const auto object_result = abstract_operations::to_object(c, argument_or_undefined(a, 0));
    if (!object_result) return object_result.error();
    if (!object_result.completion().is_normal()) return object_result.completion();
    const Value target = object_result.completion().value();

    const auto keys = c.own_property_keys(target);
    if (!keys) return keys.error();

    Value result = c.array();
    for (const PropertyKey key : *keys) {
        if (!key.is_atom()) continue;
        const auto descriptor = c.get_own_property_descriptor(target, key);
        if (!descriptor) return descriptor.error();
        if (!descriptor->has_value() || !(**descriptor).enumerable.value_or(false)) continue;
        const auto pushed = c.array_push(result, c.string(std::string(c.property_key_text(key))));
        if (!pushed) return pushed.error();
    }
    return Completion::normal(result);
}
ExecutionResult builtin_is_nan(Context& c, Value, std::span<const Value> a) { const Value v = argument_or_undefined(a, 0); return Completion::normal(c.boolean(v.is_number() && std::isnan(v.as_number()))); }
ExecutionResult builtin_promise_resolve(Context& c, Value, std::span<const Value> a) { return execution_from_result(c.promise_resolve(argument_or_undefined(a, 0))); }
ExecutionResult builtin_promise_reject(Context& c, Value, std::span<const Value> a) { return execution_from_result(c.promise_reject(argument_or_undefined(a, 0))); }
ExecutionResult builtin_promise_then(Context& c, Value t, std::span<const Value> a) { return execution_from_result(c.promise_then(t, argument_or_undefined(a, 0))); }
ExecutionResult builtin_promise_catch(Context& c, Value t, std::span<const Value> a) { return execution_from_result(c.promise_catch(t, argument_or_undefined(a, 0))); }
ExecutionResult builtin_regexp_test(Context& c, Value t, std::span<const Value> a) { return execution_from_result(c.regexp_test(t, argument_or_undefined(a, 0))); }
ExecutionResult builtin_regexp_exec(Context& c, Value t, std::span<const Value> a) { return execution_from_result(c.regexp_exec(t, argument_or_undefined(a, 0))); }
ExecutionResult builtin_symbol(Context& c, Value, std::span<const Value> a) {
    if (a.empty() || a[0].is_undefined()) return Completion::normal(c.symbol());
    const auto text = abstract_operations::to_string(c, a[0]);
    if (!text) return text.error();
    if (!text.completion().is_normal()) return text.completion();
    return Completion::normal(c.symbol(std::string(text.completion().value().as_string())));
}
ExecutionResult builtin_symbol_for(Context& c, Value, std::span<const Value> a) {
    const auto text = abstract_operations::to_string(c, argument_or_undefined(a, 0));
    if (!text) return text.error();
    if (!text.completion().is_normal()) return text.completion();
    return Completion::normal(c.symbol_for(text.completion().value().as_string()));
}
ExecutionResult builtin_symbol_key_for(Context& c, Value, std::span<const Value> a) {
    const Value value = argument_or_undefined(a, 0);
    if (!value.is_symbol()) return Error{ErrorCode::type_error, "Symbol.keyFor requires a Symbol"};
    const auto key = c.symbol_key_for(value);
    return Completion::normal(key ? c.string(*key) : c.undefined());
}
ExecutionResult builtin_symbol_to_string(Context& c, Value this_value, std::span<const Value>) {
    Value symbol = this_value;
    if (this_value.is_object()) {
        const auto* object = detail::ValueAccess::object(this_value);
        if (object->boxed_primitive && object->boxed_primitive->is_symbol()) symbol = *object->boxed_primitive;
    }
    if (!symbol.is_symbol()) return Completion::throw_(c.type_error("Symbol.prototype.toString requires a Symbol receiver"));
    return Completion::normal(c.string("Symbol(" + std::string(symbol.symbol_description()) + ")"));
}
ExecutionResult builtin_function_call(Context& c, Value target, std::span<const Value> a) {
    if (!target.is_function()) return Error{ErrorCode::type_error, "Function.prototype.call receiver is not callable"};
    const Value this_arg = argument_or_undefined(a, 0);
    const std::span<const Value> rest = a.size() > 1U ? a.subspan(1U) : std::span<const Value>{};
    return c.invoke(target, rest, this_arg);
}
ExecutionResult builtin_function_apply(Context& c, Value target, std::span<const Value> a) {
    if (!target.is_function()) return Error{ErrorCode::type_error, "Function.prototype.apply receiver is not callable"};
    const Value this_arg = argument_or_undefined(a, 0);
    const Value array_like = argument_or_undefined(a, 1);
    if (array_like.is_undefined() || array_like.is_null()) return c.invoke(target, {}, this_arg);
    if (!array_like.is_object_like()) return Error{ErrorCode::type_error, "Function.prototype.apply argumentsList is not an object"};
    const auto length_value = c.get_property_semantic(array_like, c.property_key("length"));
    if (!length_value) return length_value.error();
    if (!length_value.completion().is_normal()) return length_value.completion();
    const auto length = abstract_operations::to_length(c, length_value.completion().value());
    if (!length) return length.error();
    if (!length.completion().is_normal()) return length.completion();
    const auto count = static_cast<std::size_t>(length.completion().value().as_number());
    std::vector<Value> values; values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto item = c.get_property_semantic(array_like, c.property_key(std::to_string(i)));
        if (!item) return item.error();
        if (!item.completion().is_normal()) return item.completion();
        values.push_back(item.completion().value());
    }
    return c.invoke(target, values, this_arg);
}
ExecutionResult builtin_function_bind(Context& c, Value target, std::span<const Value> a) {
    if (!target.is_function()) return Error{ErrorCode::type_error, "Function.prototype.bind receiver is not callable"};
    const Value this_arg = argument_or_undefined(a, 0);
    const std::span<const Value> bound = a.size() > 1U ? a.subspan(1U) : std::span<const Value>{};
    const auto result = c.bind_function(target, this_arg, bound);
    if (!result) return result.error();
    return Completion::normal(*result);
}
ExecutionResult builtin_object_value_of(Context&, Value this_value, std::span<const Value>) {
    if (this_value.is_object()) {
        auto* object = detail::ValueAccess::object(this_value);
        if (object->boxed_primitive) return Completion::normal(*object->boxed_primitive);
    }
    return Completion::normal(this_value);
}
ExecutionResult builtin_object_to_string(Context& c, Value this_value, std::span<const Value>) {
    if (this_value.is_undefined()) return Completion::normal(c.string("[object Undefined]"));
    if (this_value.is_null()) return Completion::normal(c.string("[object Null]"));
    if (this_value.is_object() && detail::ValueAccess::object(this_value)->is_array)
        return Completion::normal(c.string("[object Array]"));
    return Completion::normal(c.string("[object Object]"));
}

std::unordered_map<PropertyKey, PropertyDescriptor, PropertyKeyHash>* property_table(Value value) {
    if (value.is_object()) return &detail::ValueAccess::object(value)->properties;
    if (value.is_function()) return &const_cast<detail::HeapFunction*>(detail::ValueAccess::function(value))->properties;
    return nullptr;
}

const std::unordered_map<PropertyKey, PropertyDescriptor, PropertyKeyHash>* property_table_const(Value value) {
    if (value.is_object()) return &detail::ValueAccess::object(value)->properties;
    if (value.is_function()) return &detail::ValueAccess::function(value)->properties;
    return nullptr;
}

Value prototype_of(Value value) {
    if (value.is_object()) return detail::ValueAccess::object(value)->prototype;
    if (value.is_function()) return detail::ValueAccess::function(value)->prototype;
    return Value::undefined();
}

void set_internal_prototype(Value value, Value prototype) {
    if (value.is_object()) detail::ValueAccess::object(value)->prototype = prototype;
    else if (value.is_function()) const_cast<detail::HeapFunction*>(detail::ValueAccess::function(value))->prototype = prototype;
}

std::optional<std::uint32_t> canonical_array_index(std::string_view key) {
    if (key.empty()) return std::nullopt;
    if (key.size() > 1U && key.front() == '0') return std::nullopt;
    std::uint64_t index = 0;
    const auto parsed = std::from_chars(key.data(), key.data() + key.size(), index, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size()) return std::nullopt;
    if (index >= 0xFFFFFFFFULL) return std::nullopt;
    return static_cast<std::uint32_t>(index);
}

detail::HeapUpvalue* mapped_argument_upvalue(Value object, std::optional<std::uint32_t> index) {
    if (!object.is_object() || !index) return nullptr;
    auto* heap = detail::ValueAccess::object(object);
    if (heap->object_kind != ObjectKind::MappedArguments || *index >= heap->mapped_argument_upvalues.size()) return nullptr;
    return heap->mapped_argument_upvalues[*index];
}

bool dense_compatible_descriptor(const PropertyDescriptor& descriptor) {
    if (descriptor.is_accessor_descriptor()) return false;
    return descriptor.value.has_value()
        && descriptor.writable.value_or(false)
        && descriptor.enumerable.value_or(false)
        && descriptor.configurable.value_or(false);
}


bool same_value(Value left, Value right) noexcept {
    if (left.tag() != right.tag()) return false;
    if (left.is_undefined() || left.is_null()) return true;
    if (left.is_boolean()) return left.as_boolean() == right.as_boolean();
    if (left.is_number()) {
        const double lhs = left.as_number();
        const double rhs = right.as_number();
        if (std::isnan(lhs) && std::isnan(rhs)) return true;
        if (lhs == 0.0 && rhs == 0.0) return std::signbit(lhs) == std::signbit(rhs);
        return lhs == rhs;
    }
    if (left.is_string()) return left.as_string() == right.as_string();
    if (left.is_symbol()) return left.as_symbol_id() == right.as_symbol_id();
    return left.same_heap_identity(right);
}

PropertyDescriptor complete_descriptor(PropertyDescriptor descriptor, bool accessor) {
    if (accessor) {
        descriptor.value.reset();
        descriptor.writable.reset();
        if (!descriptor.get) descriptor.get = Value::undefined();
        if (!descriptor.set) descriptor.set = Value::undefined();
    } else {
        descriptor.get.reset();
        descriptor.set.reset();
        if (!descriptor.value) descriptor.value = Value::undefined();
        if (!descriptor.writable) descriptor.writable = false;
    }
    if (!descriptor.enumerable) descriptor.enumerable = false;
    if (!descriptor.configurable) descriptor.configurable = false;
    return descriptor;
}

bool descriptor_callable_or_undefined(const std::optional<Value>& value) noexcept {
    return !value || value->is_undefined() || value->is_function();
}

bool is_extensible_value(Value value) noexcept {
    if (value.is_object()) return detail::ValueAccess::object(value)->extensible;
    if (value.is_function()) return detail::ValueAccess::function(value)->extensible;
    return false;
}

void set_extensible_value(Value value, bool extensible) noexcept {
    if (value.is_object()) detail::ValueAccess::object(value)->extensible = extensible;
    else if (value.is_function()) detail::ValueAccess::mutable_function(value)->extensible = extensible;
}
}

Context::Context(Runtime& runtime) noexcept : runtime_(&runtime), realm_(&runtime.create_realm()) {
    runtime_->register_context();
    ensure_builtins(*realm_);
}

Context::~Context() {
    runtime_->unregister_context();
}

ExecutionResult Context::invoke(Value callee, std::span<const Value> arguments, Value this_value) {
    if (runtime_->active_vms_.empty()) {
        VM vm(*this);
        return vm.invoke(callee, this_value, arguments);
    }
    return runtime_->active_vms_.back()->invoke(callee, this_value, arguments);
}

ExecutionResult Context::construct(Value constructor, std::span<const Value> arguments) {
    if (runtime_->active_vms_.empty()) {
        VM vm(*this);
        return vm.construct(constructor, arguments);
    }
    return runtime_->active_vms_.back()->construct(constructor, arguments);
}

ExecutionResult Context::resume_generator(Value generator, Value input) {
    if (runtime_->active_vms_.empty()) {
        VM vm(*this);
        return vm.resume_generator(generator, input);
    }
    return runtime_->active_vms_.back()->resume_generator(generator, input);
}

Result<PropertyKey> Context::property_key(Value key) const {
    const auto validation = validate(key);
    if (!validation) return validation.error();
    if (key.is_symbol()) return PropertyKey::symbol(key.as_symbol_id());
    if (!key.is_string()) return Error{ErrorCode::type_error, "property_key(Value) requires String or Symbol"};
    return PropertyKey::atom(runtime_->intern_atom(key.as_string()));
}

Result<void> Context::validate(const Value& value) const {
    if (runtime_->owns(value)) return {};
    return Error{ErrorCode::runtime_mismatch, "heap-backed Value belongs to a different Runtime"};
}

Value Context::object() {
    return object_in_realm(active_realm());
}

Value Context::box_primitive(Value primitive) {
    const auto validation = validate(primitive);
    if (!validation || primitive.is_object_like() || primitive.is_null() || primitive.is_undefined()) return Value::undefined();
    Realm& realm = active_realm();
    Value prototype = realm.object_prototype_;
    if (primitive.is_symbol() && realm.symbol_prototype_.is_object_like()) prototype = realm.symbol_prototype_;
    const auto boxed_result = object_in_realm(realm, prototype);
    if (!boxed_result) return Value::undefined();
    Value boxed = *boxed_result;
    boxed.as_heap_object()->boxed_primitive = primitive;
    if (primitive.is_string()) boxed.as_heap_object()->object_kind = ObjectKind::StringExotic;
    return boxed;
}

Value Context::object_in_realm(Realm& realm) {
    ensure_builtins(realm);
    Value value = runtime_->make_object(realm);
    value.as_heap_object()->prototype = realm.object_prototype_;
    return value;
}

Value Context::object_prototype() {
    ensure_builtins();
    return active_realm().object_prototype_;
}

Value Context::error(std::string_view message) {
    ensure_builtins();
    return make_error_instance(*this, active_realm().error_prototype_, "Error", message);
}

Value Context::type_error(std::string_view message) {
    ensure_builtins();
    return make_error_instance(*this, active_realm().type_error_prototype_, "TypeError", message);
}

Value Context::reference_error(std::string_view message) {
    ensure_builtins();
    return make_error_instance(*this, active_realm().reference_error_prototype_, "ReferenceError", message);
}

Value Context::native_function(std::string_view name, std::uint32_t arity, NativeFunction function, ConstructorKind constructor_kind) {
    return native_function_in_realm(active_realm(), name, arity, function, constructor_kind);
}

Result<Value> Context::bind_function(Value target, Value bound_this, std::span<const Value> bound_arguments) {
    const auto tv = validate(target); if (!tv) return tv.error();
    const auto bv = validate(bound_this); if (!bv) return bv.error();
    if (!target.is_function()) return Error{ErrorCode::type_error, "bind target is not callable"};
    for (Value value : bound_arguments) { const auto v = validate(value); if (!v) return v.error(); }
    return runtime_->make_bound_function(*target.as_heap_function()->realm, target, bound_this, bound_arguments);
}

Value Context::native_function_in_realm(Realm& realm, std::string_view name, std::uint32_t arity, NativeFunction function, ConstructorKind constructor_kind) {
    return runtime_->make_native_function(realm, std::string(name), arity, function, constructor_kind);
}

Result<Value> Context::object(Value prototype) {
    return object_in_realm(active_realm(), prototype);
}

Result<Value> Context::object_in_realm(Realm& realm, Value prototype) {
    const auto validation = validate(prototype);
    if (!validation) return validation.error();
    if (!prototype.is_null() && !prototype.is_object_like()) {
        return Error{ErrorCode::type_error, "object prototype must be null or object"};
    }
    Value object_value = runtime_->make_object(realm);
    object_value.as_heap_object()->prototype = prototype;
    return object_value;
}

ObjectKind Context::object_kind(const Value& object) const {
    if (object.is_function()) return ObjectKind::Function;
    if (!object.is_object()) return ObjectKind::Ordinary;
    const auto* heap = object.as_heap_object();
    if (heap->is_generator) return ObjectKind::Generator;
    if (heap->is_regexp) return ObjectKind::RegExp;
    if (heap->is_promise) return ObjectKind::Promise;
    if (heap->is_array) return ObjectKind::Array;
    return heap->object_kind;
}

Result<std::optional<PropertyDescriptor>> Context::get_own_property_descriptor(const Value& object, PropertyKey key) const {
    const auto object_validation = validate(object);
    if (!object_validation) return object_validation.error();
    const auto* properties = property_table_const(object);
    if (properties == nullptr) return Error{ErrorCode::type_error, "property access target is not an object"};

    const auto found = properties->find(key);
    if (found != properties->end()) {
        PropertyDescriptor descriptor = found->second;
        if (object.is_object() && key.is_atom()) {
            const auto index = canonical_array_index(runtime_->atom_text(key.atom_id()));
            if (auto* mapped = mapped_argument_upvalue(object, index)) descriptor.value = mapped->get();
        }
        return std::optional<PropertyDescriptor>{descriptor};
    }

    if (object.is_object() && object.as_heap_object()->is_array) {
        const auto* array = object.as_heap_object();
        if ((key.is_atom() && runtime_->atom_text(key.atom_id()) == "length")) {
            return std::optional<PropertyDescriptor>{PropertyDescriptor::data(
                Value::number(static_cast<double>(array->array_length)), array->array_length_writable, false, false)};
        }
        if (const auto index = (key.is_atom() ? canonical_array_index(runtime_->atom_text(key.atom_id())) : std::nullopt)) {
            if (*index < array->elements.size() && array->elements[*index].has_value()) {
                return std::optional<PropertyDescriptor>{PropertyDescriptor::data(*array->elements[*index], true, true, true)};
            }
        }
    }

    if (key.is_atom() && object.is_function()) {
        const std::string_view name = runtime_->atom_text(key.atom_id());
        if (name == "length") return std::optional<PropertyDescriptor>{PropertyDescriptor::data(Value::number(static_cast<double>(object.as_heap_function()->code->arity)), false, false, true)};
        if (name == "name") return std::optional<PropertyDescriptor>{PropertyDescriptor::data(runtime_->make_string(object.as_heap_function()->code->name), false, false, true)};
    }
    return std::optional<PropertyDescriptor>{};
}

Result<bool> Context::define_own_property(const Value& object, PropertyKey key, const PropertyDescriptor& descriptor) const {
    const auto object_validation = validate(object);
    if (!object_validation) return object_validation.error();
    auto* properties = property_table(object);
    if (properties == nullptr) return Error{ErrorCode::type_error, "property definition target is not an object"};
    if (descriptor.is_data_descriptor() && descriptor.is_accessor_descriptor())
        return Error{ErrorCode::type_error, "property descriptor cannot be both data and accessor"};
    if (!descriptor_callable_or_undefined(descriptor.get) || !descriptor_callable_or_undefined(descriptor.set))
        return Error{ErrorCode::type_error, "property getter/setter must be callable or undefined"};
    if (descriptor.value) { const auto v = validate(*descriptor.value); if (!v) return v.error(); }
    if (descriptor.get) { const auto v = validate(*descriptor.get); if (!v) return v.error(); }
    if (descriptor.set) { const auto v = validate(*descriptor.set); if (!v) return v.error(); }

    if (object.is_object() && object.as_heap_object()->is_array) {
        auto* array = object.as_heap_object();
        if ((key.is_atom() && runtime_->atom_text(key.atom_id()) == "length")) {
            if (descriptor.is_accessor_descriptor()) return false;
            if (descriptor.enumerable.value_or(false) || descriptor.configurable.value_or(false)) return false;
            if (descriptor.writable.value_or(false) && !array->array_length_writable) return false;
            if (!descriptor.value) {
                if (descriptor.writable && !*descriptor.writable) array->array_length_writable = false;
                return true;
            }
            if (!descriptor.value->is_number()) return Error{ErrorCode::type_error, "array length must be a uint32 value"};
            const double number = descriptor.value->as_number();
            if (!std::isfinite(number) || number < 0.0 || std::floor(number) != number || number > 4294967295.0)
                return Error{ErrorCode::type_error, "invalid array length"};
            const auto new_length = static_cast<std::uint32_t>(number);
            const auto old_length = array->array_length;
            if (new_length >= old_length) {
                if (new_length != old_length && !array->array_length_writable) return false;
                array->array_length = new_length;
                if (descriptor.writable && !*descriptor.writable) array->array_length_writable = false;
                return true;
            }
            if (!array->array_length_writable) return false;

            // ArraySetLength deletes indexed properties from the end. Dense entries are
            // configurable by construction; descriptor fallbacks may block the shrink.
            std::uint32_t blocked_length = new_length;
            bool blocked = false;
            for (const PropertyKey candidate : array->property_order) {
                const auto candidate_index = (candidate.is_atom() ? canonical_array_index(runtime_->atom_text(candidate.atom_id())) : std::nullopt);
                if (!candidate_index || *candidate_index < new_length || *candidate_index >= old_length) continue;
                const auto it = array->properties.find(candidate);
                if (it != array->properties.end() && !it->second.configurable.value_or(false)) {
                    blocked = true;
                    blocked_length = std::max(blocked_length, static_cast<std::uint32_t>(*candidate_index + 1U));
                }
            }
            const std::uint32_t target_length = blocked ? blocked_length : new_length;
            for (auto it = array->properties.begin(); it != array->properties.end();) {
                const auto candidate_index = (it->first.is_atom() ? canonical_array_index(runtime_->atom_text(it->first.atom_id())) : std::nullopt);
                if (candidate_index && *candidate_index >= target_length && *candidate_index < old_length && it->second.configurable.value_or(false)) {
                    const PropertyKey erased = it->first;
                    it = array->properties.erase(it);
                    array->property_order.erase(std::remove(array->property_order.begin(), array->property_order.end(), erased), array->property_order.end());
                } else ++it;
            }
            if (array->elements.size() > target_length) array->elements.resize(target_length);
            array->array_length = target_length;
            if (descriptor.writable && !*descriptor.writable) array->array_length_writable = false;
            return !blocked;
        }

        if (const auto index = (key.is_atom() ? canonical_array_index(runtime_->atom_text(key.atom_id())) : std::nullopt)) {
            const bool has_dense = *index < array->elements.size() && array->elements[*index].has_value();
            auto exceptional = properties->find(key);
            const bool exists = has_dense || exceptional != properties->end();
            if (!exists) {
                if (!is_extensible_value(object)) return false;
                if (*index >= array->array_length && !array->array_length_writable) return false;
                const bool keep_dense = dense_compatible_descriptor(descriptor)
                    && *index <= array->elements.size() + 1024U && *index < (1U << 20U);
                if (keep_dense) {
                    if (array->elements.size() <= *index) array->elements.resize(static_cast<std::size_t>(*index) + 1U);
                    array->elements[*index] = descriptor.value.value_or(Value::undefined());
                } else {
                    properties->emplace(key, complete_descriptor(descriptor, descriptor.is_accessor_descriptor()));
                    array->property_order.push_back(key);
                }
                if (*index >= array->array_length) array->array_length = *index + 1U;
                return true;
            }

            if (has_dense && exceptional == properties->end()) {
                if (dense_compatible_descriptor(descriptor)) {
                    if (descriptor.value) array->elements[*index] = *descriptor.value;
                    return true;
                }
                properties->emplace(key, PropertyDescriptor::data(*array->elements[*index], true, true, true));
                array->property_order.push_back(key);
                array->elements[*index].reset();
            }
        }
    }

    auto found = properties->find(key);
    if (found == properties->end()) {
        if (!is_extensible_value(object)) return false;
        const bool accessor = descriptor.is_accessor_descriptor();
        properties->emplace(key, complete_descriptor(descriptor, accessor));
        if (object.is_object()) object.as_heap_object()->property_order.push_back(key);
        else const_cast<detail::HeapFunction*>(object.as_heap_function())->property_order.push_back(key);
        return true;
    }

    PropertyDescriptor current = found->second;
    std::optional<std::uint32_t> mapped_index;
    detail::HeapUpvalue* mapped = nullptr;
    if (object.is_object() && key.is_atom()) {
        mapped_index = canonical_array_index(runtime_->atom_text(key.atom_id()));
        mapped = mapped_argument_upvalue(object, mapped_index);
        if (mapped != nullptr && !current.is_accessor_descriptor()) current.value = mapped->get();
    }
    if (descriptor.empty()) return true;
    const bool current_configurable = current.configurable.value_or(false);
    if (!current_configurable) {
        if (descriptor.configurable.value_or(false)) return false;
        if (descriptor.enumerable && *descriptor.enumerable != current.enumerable.value_or(false)) return false;
    }

    const bool current_accessor = current.is_accessor_descriptor();
    const bool incoming_accessor = descriptor.is_accessor_descriptor();
    const bool incoming_data = descriptor.is_data_descriptor();
    if (!descriptor.is_generic_descriptor() && current_accessor != incoming_accessor) {
        if (!current_configurable) return false;
        current = complete_descriptor(PropertyDescriptor{}, incoming_accessor);
    } else if (!current_accessor && incoming_data && !current_configurable && !current.writable.value_or(false)) {
        if (descriptor.writable.value_or(false)) return false;
        if (descriptor.value && !same_value(*descriptor.value, current.value.value_or(Value::undefined()))) return false;
    } else if (current_accessor && incoming_accessor && !current_configurable) {
        if (descriptor.get && !same_value(*descriptor.get, current.get.value_or(Value::undefined()))) return false;
        if (descriptor.set && !same_value(*descriptor.set, current.set.value_or(Value::undefined()))) return false;
    }

    if (descriptor.value) current.value = descriptor.value;
    if (descriptor.writable) current.writable = descriptor.writable;
    if (descriptor.get) current.get = descriptor.get;
    if (descriptor.set) current.set = descriptor.set;
    if (descriptor.enumerable) current.enumerable = descriptor.enumerable;
    if (descriptor.configurable) current.configurable = descriptor.configurable;
    if (mapped != nullptr) {
        if (descriptor.value) mapped->set(*descriptor.value);
        if (descriptor.is_accessor_descriptor() || (descriptor.writable && !*descriptor.writable)) {
            object.as_heap_object()->mapped_argument_upvalues[*mapped_index] = nullptr;
        }
    }
    found->second = complete_descriptor(current, current.is_accessor_descriptor());
    return true;
}

Result<bool> Context::has_property(const Value& object, PropertyKey key) const {
    const auto object_validation = validate(object);
    if (!object_validation) return object_validation.error();
    if (!object.is_object_like()) return Error{ErrorCode::type_error, "property target is not an object"};
    Value current = object;
    std::unordered_set<const detail::HeapCell*> visited;
    while (current.is_object_like()) {
        const auto own = get_own_property_descriptor(current, key); if (!own) return own.error();
        if (own->has_value()) return true;
        const detail::HeapCell* cell = current.is_object() ? static_cast<const detail::HeapCell*>(current.as_heap_object()) : static_cast<const detail::HeapCell*>(current.as_heap_function());
        if (!visited.insert(cell).second) return Error{ErrorCode::internal, "prototype chain contains a cycle"};
        current = prototype_of(current);
    }
    if (!current.is_null()) return Error{ErrorCode::internal, "object prototype is neither object nor null"};
    return false;
}

Result<bool> Context::delete_property(const Value& object, PropertyKey key) const {
    const auto object_validation = validate(object); if (!object_validation) return object_validation.error();
    auto* properties = property_table(object);
    if (properties == nullptr) return Error{ErrorCode::type_error, "property deletion target is not an object"};
    if (object.is_object() && object.as_heap_object()->is_array) {
        auto* array = object.as_heap_object();
        if ((key.is_atom() && runtime_->atom_text(key.atom_id()) == "length")) return false;
        if (const auto index = (key.is_atom() ? canonical_array_index(runtime_->atom_text(key.atom_id())) : std::nullopt)) {
            const auto found = properties->find(key);
            if (found != properties->end()) {
                if (!found->second.configurable.value_or(false)) return false;
                properties->erase(found);
                array->property_order.erase(std::remove(array->property_order.begin(), array->property_order.end(), key), array->property_order.end());
                return true;
            }
            if (*index < array->elements.size()) array->elements[*index].reset();
            return true;
        }
    }
    const auto found = properties->find(key);
    if (found == properties->end()) return true;
    if (!found->second.configurable.value_or(false)) return false;
    if (object.is_object() && key.is_atom()) {
        const auto index = canonical_array_index(runtime_->atom_text(key.atom_id()));
        if (index && object.as_heap_object()->object_kind == ObjectKind::MappedArguments && *index < object.as_heap_object()->mapped_argument_upvalues.size())
            object.as_heap_object()->mapped_argument_upvalues[*index] = nullptr;
    }
    properties->erase(found);
    auto& order = object.is_object() ? object.as_heap_object()->property_order : const_cast<detail::HeapFunction*>(object.as_heap_function())->property_order;
    order.erase(std::remove(order.begin(), order.end(), key), order.end());
    return true;
}

Result<std::vector<PropertyKey>> Context::own_property_keys(const Value& object) const {
    const auto object_validation = validate(object); if (!object_validation) return object_validation.error();
    if (!object.is_object_like()) return Error{ErrorCode::type_error, "property key target is not an object"};
    if (object.is_object() && object.as_heap_object()->is_array) {
        const auto* array = object.as_heap_object();
        std::vector<std::pair<std::uint32_t, PropertyKey>> indexed;
        for (std::uint32_t i = 0; i < array->elements.size(); ++i) {
            if (array->elements[i]) indexed.emplace_back(i, PropertyKey::atom(runtime_->intern_atom(std::to_string(i))));
        }
        for (const auto& [candidate, descriptor] : array->properties) {
            (void)descriptor;
            if (const auto index = (candidate.is_atom() ? canonical_array_index(runtime_->atom_text(candidate.atom_id())) : std::nullopt)) indexed.emplace_back(*index, candidate);
        }
        std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<PropertyKey> keys;
        keys.reserve(indexed.size() + 1U + array->property_order.size());
        for (const auto& [index, candidate] : indexed) { (void)index; keys.push_back(candidate); }
        keys.push_back(PropertyKey::atom(runtime_->intern_atom("length")));
        for (const PropertyKey candidate : array->property_order) {
            if (!(candidate.is_atom() ? canonical_array_index(runtime_->atom_text(candidate.atom_id())) : std::nullopt)) keys.push_back(candidate);
        }
        return keys;
    }
    const auto& order = object.is_object()
        ? object.as_heap_object()->property_order
        : object.as_heap_function()->property_order;
    std::vector<std::pair<std::uint32_t, PropertyKey>> indexed;
    std::vector<PropertyKey> strings;
    std::vector<PropertyKey> symbols;
    indexed.reserve(order.size());
    strings.reserve(order.size());
    symbols.reserve(order.size());
    for (const PropertyKey candidate : order) {
        if (candidate.is_symbol()) {
            symbols.push_back(candidate);
            continue;
        }
        if (const auto index = canonical_array_index(runtime_->atom_text(candidate.atom_id())))
            indexed.emplace_back(*index, candidate);
        else
            strings.push_back(candidate);
    }
    std::sort(indexed.begin(), indexed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<PropertyKey> keys;
    keys.reserve(order.size());
    for (const auto& [index, candidate] : indexed) { (void)index; keys.push_back(candidate); }
    keys.insert(keys.end(), strings.begin(), strings.end());
    keys.insert(keys.end(), symbols.begin(), symbols.end());
    return keys;
}

Result<bool> Context::is_extensible(const Value& object) const {
    const auto validation = validate(object); if (!validation) return validation.error();
    if (!object.is_object_like()) return Error{ErrorCode::type_error, "extensibility target is not an object"};
    return is_extensible_value(object);
}

Result<bool> Context::prevent_extensions(const Value& object) const {
    const auto validation = validate(object); if (!validation) return validation.error();
    if (!object.is_object_like()) return Error{ErrorCode::type_error, "extensibility target is not an object"};
    set_extensible_value(object, false);
    return true;
}

Result<Value> Context::get_own_property(const Value& object, PropertyKey key) const {
    const auto descriptor = get_own_property_descriptor(object, key); if (!descriptor) return descriptor.error();
    if (!descriptor->has_value()) return Value::undefined();
    if ((*descriptor)->is_accessor_descriptor()) return Value::undefined();
    return (*descriptor)->value.value_or(Value::undefined());
}

ExecutionResult Context::get_property_semantic(const Value& object, PropertyKey key, Value receiver) {
    const auto object_validation = validate(object); if (!object_validation) return object_validation.error();
    const auto receiver_validation = validate(receiver); if (!receiver_validation) return receiver_validation.error();
    if (object.is_string() && key.is_atom() && runtime_->atom_text(key.atom_id()) == "length") {
        return Completion::normal(Value::number(static_cast<double>(object.as_string().size())));
    }
    if (!object.is_object_like()) {
        if (object.is_undefined() || object.is_null()) return Completion::throw_(type_error("cannot read property of null or undefined"));
        const Value boxed = box_primitive(object);
        if (boxed.is_undefined()) return EngineFailure{EngineFailureCode::InternalInvariant, "failed to box primitive for property access"};
        return get_property_semantic(boxed, key, receiver);
    }

    const auto own = get_own_property_descriptor(object, key); if (!own) return own.error();
    if (own->has_value()) {
        const PropertyDescriptor& descriptor = **own;
        if (!descriptor.is_accessor_descriptor()) return Completion::normal(descriptor.value.value_or(Value::undefined()));
        const Value getter = descriptor.get.value_or(Value::undefined());
        if (getter.is_undefined()) return Completion::normal(Value::undefined());
        return invoke(getter, {}, receiver);
    }
    const Value parent = prototype_of(object);
    if (parent.is_null()) return Completion::normal(Value::undefined());
    if (!parent.is_object_like()) return EngineFailure{EngineFailureCode::InternalInvariant, "object prototype is neither object nor null"};
    return get_property_semantic(parent, key, receiver);
}

Result<Value> Context::get_property(const Value& object, PropertyKey key) {
    const ExecutionResult result = get_property_semantic(object, key, object);
    if (!result) return result.error().legacy_error();
    if (result.completion().is_throw()) return Error{ErrorCode::uncaught_exception, "uncaught JavaScript exception: " + result.completion().value().to_debug_string()};
    if (!result.completion().is_normal()) return Error{ErrorCode::internal, "non-normal completion escaped property get"};
    return result.completion().value();
}

Result<void> Context::set_own_property(const Value& object, PropertyKey key, Value value) const {
    const auto value_validation = validate(value); if (!value_validation) return value_validation.error();
    const auto current = get_own_property_descriptor(object, key); if (!current) return current.error();
    PropertyDescriptor descriptor;
    descriptor.value = value;
    if (!current->has_value()) {
        descriptor.writable = true;
        descriptor.enumerable = true;
        descriptor.configurable = true;
    }
    const auto defined = define_own_property(object, key, descriptor); if (!defined) return defined.error();
    if (!*defined) return Error{ErrorCode::type_error, "property is not writable or object is not extensible"};
    return {};
}

ExecutionResult Context::set_property_semantic(const Value& object, PropertyKey key, Value value, Value receiver) {
    const auto object_validation = validate(object); if (!object_validation) return object_validation.error();
    const auto value_validation = validate(value); if (!value_validation) return value_validation.error();
    const auto receiver_validation = validate(receiver); if (!receiver_validation) return receiver_validation.error();
    // PutValue for a property Reference reaches [[Set]] only after the RHS has
    // been evaluated.  Preserve that boundary here: null/undefined are
    // ECMAScript TypeErrors, while other primitive bases are boxed for the
    // property lookup with the original primitive retained as the Receiver.
    if (!object.is_object_like()) {
        if (object.is_undefined() || object.is_null())
            return Completion::throw_(type_error("cannot set property of null or undefined"));
        const Value boxed = box_primitive(object);
        if (boxed.is_undefined())
            return EngineFailure{EngineFailureCode::InternalInvariant, "failed to box primitive for property write"};
        return set_property_semantic(boxed, key, value, receiver);
    }

    auto own = get_own_property_descriptor(object, key); if (!own) return own.error();
    if (!own->has_value()) {
        const Value parent = prototype_of(object);
        if (!parent.is_null()) {
            if (!parent.is_object_like()) return EngineFailure{EngineFailureCode::InternalInvariant, "object prototype is neither object nor null"};
            return set_property_semantic(parent, key, value, receiver);
        }
        own = std::optional<PropertyDescriptor>{PropertyDescriptor::data(Value::undefined(), true, true, true)};
    }

    const PropertyDescriptor& descriptor = **own;
    if (!descriptor.is_accessor_descriptor()) {
        if (!descriptor.writable.value_or(false)) return Completion::normal(Value::boolean(false));
        if (!receiver.is_object_like()) return Completion::normal(Value::boolean(false));
        const auto receiver_desc = get_own_property_descriptor(receiver, key); if (!receiver_desc) return receiver_desc.error();
        if (receiver_desc->has_value()) {
            if ((*receiver_desc)->is_accessor_descriptor() || !(*receiver_desc)->writable.value_or(false)) return Completion::normal(Value::boolean(false));
            PropertyDescriptor update; update.value = value;
            const auto defined = define_own_property(receiver, key, update); if (!defined) return defined.error();
            return Completion::normal(Value::boolean(*defined));
        }
        const auto created = define_own_property(receiver, key, PropertyDescriptor::data(value, true, true, true)); if (!created) return created.error();
        return Completion::normal(Value::boolean(*created));
    }

    const Value setter = descriptor.set.value_or(Value::undefined());
    if (setter.is_undefined()) return Completion::normal(Value::boolean(false));
    const Value argument = value;
    const ExecutionResult invoked = invoke(setter, std::span<const Value>(&argument, 1U), receiver);
    if (!invoked) return invoked.error();
    if (invoked.completion().is_throw()) return invoked.completion();
    if (!invoked.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "setter produced non-call completion"};
    return Completion::normal(Value::boolean(true));
}

Result<void> Context::set_property(const Value& object, PropertyKey key, Value value) {
    const ExecutionResult result = set_property_semantic(object, key, value, object);
    if (!result) return result.error().legacy_error();
    if (result.completion().is_throw()) return Error{ErrorCode::uncaught_exception, "uncaught JavaScript exception: " + result.completion().value().to_debug_string()};
    if (!result.completion().is_normal()) return Error{ErrorCode::internal, "non-normal completion escaped property set"};
    if (!result.completion().value().is_boolean() || !result.completion().value().as_boolean()) return Error{ErrorCode::type_error, "property is not writable"};
    return {};
}

Result<Value> Context::function_bound_target(const Value& function) const {
    const auto validation = validate(function); if (!validation) return validation.error();
    if (!function.is_function()) return Error{ErrorCode::type_error, "bound-target query requires a function"};
    return detail::ValueAccess::function(function)->bound_target;
}

Result<Value> Context::get_prototype(const Value& object) const {
    const auto validation = validate(object);
    if (!validation) return validation.error();
    if (!object.is_object_like()) return Error{ErrorCode::type_error, "prototype target is not an object"};
    return prototype_of(object);
}

Result<void> Context::set_prototype(const Value& object, Value prototype) const {
    const auto object_validation = validate(object);
    if (!object_validation) return object_validation.error();
    const auto prototype_validation = validate(prototype);
    if (!prototype_validation) return prototype_validation.error();
    if (!object.is_object_like()) return Error{ErrorCode::type_error, "prototype target is not an object"};
    if (!prototype.is_null() && !prototype.is_object_like()) return Error{ErrorCode::type_error, "prototype must be null or object"};
    const Value current_prototype = prototype_of(object);
    if (same_value(current_prototype, prototype)) return {};
    if (!is_extensible_value(object)) return Error{ErrorCode::type_error, "cannot change prototype of non-extensible object"};

    const detail::HeapCell* target = object.is_object()
        ? static_cast<const detail::HeapCell*>(object.as_heap_object())
        : static_cast<const detail::HeapCell*>(object.as_heap_function());
    Value current = prototype;
    std::unordered_set<const detail::HeapCell*> visited;
    while (current.is_object_like()) {
        const detail::HeapCell* candidate = current.is_object()
            ? static_cast<const detail::HeapCell*>(current.as_heap_object())
            : static_cast<const detail::HeapCell*>(current.as_heap_function());
        if (candidate == target) return Error{ErrorCode::type_error, "prototype cycle is not allowed"};
        if (!visited.insert(candidate).second) return Error{ErrorCode::type_error, "prototype chain already contains a cycle"};
        current = prototype_of(current);
    }
    set_internal_prototype(object, prototype);
    return {};
}

void Context::ensure_builtins() { ensure_builtins(active_realm()); }

void Context::ensure_builtins(Realm& realm) {
    if (realm.builtins_initialized_) return;
    realm.builtins_initialized_ = true;
    realm.object_prototype_ = runtime_->make_object(realm);
    // %Function.prototype% is itself callable.  Create it before the rest of
    // the native intrinsics, then explicitly attach %Object.prototype% as its
    // [[Prototype]].
    realm.function_prototype_ = runtime_->make_native_function(
        realm, "", 0, builtin_function_prototype, ConstructorKind::None);
    detail::ValueAccess::mutable_function(realm.function_prototype_)->prototype = realm.object_prototype_;
    realm.error_prototype_ = runtime_->make_object(realm);
    realm.error_prototype_.as_heap_object()->prototype = realm.object_prototype_;
    realm.type_error_prototype_ = runtime_->make_object(realm);
    realm.type_error_prototype_.as_heap_object()->prototype = realm.error_prototype_;
    realm.reference_error_prototype_ = runtime_->make_object(realm);
    realm.reference_error_prototype_.as_heap_object()->prototype = realm.error_prototype_;
    realm.array_prototype_ = runtime_->make_array(realm);
    realm.promise_prototype_ = runtime_->make_object(realm);
    realm.regexp_prototype_ = runtime_->make_object(realm);
    realm.symbol_prototype_ = runtime_->make_object(realm);
    realm.array_prototype_.as_heap_object()->prototype = realm.object_prototype_;
    realm.promise_prototype_.as_heap_object()->prototype = realm.object_prototype_;
    realm.regexp_prototype_.as_heap_object()->prototype = realm.object_prototype_;
    realm.symbol_prototype_.as_heap_object()->prototype = realm.object_prototype_;
    realm.global_object_.as_heap_object()->prototype = realm.object_prototype_;
    (void)set_own_property(realm.object_prototype_, "valueOf", native_function_in_realm(realm, "valueOf", 0, builtin_object_value_of));
    (void)set_own_property(realm.object_prototype_, "toString", native_function_in_realm(realm, "toString", 0, builtin_object_to_string));
    (void)set_own_property(realm.object_prototype_, "hasOwnProperty", native_function_in_realm(realm, "hasOwnProperty", 1, builtin_object_has_own_property));
    (void)set_own_property(realm.object_prototype_, "propertyIsEnumerable", native_function_in_realm(realm, "propertyIsEnumerable", 1, builtin_object_property_is_enumerable));
    (void)set_own_property(realm.function_prototype_, "call", native_function_in_realm(realm, "call", 1, builtin_function_call));
    (void)set_own_property(realm.function_prototype_, "apply", native_function_in_realm(realm, "apply", 2, builtin_function_apply));
    (void)set_own_property(realm.function_prototype_, "bind", native_function_in_realm(realm, "bind", 1, builtin_function_bind));
    Value function_ns = native_function_in_realm(
        realm, "Function", 1, builtin_function_constructor, ConstructorKind::Base);
    (void)define_own_property(function_ns, "prototype",
                              PropertyDescriptor::data(realm.function_prototype_, false, false, false));
    (void)define_own_property(realm.function_prototype_, "constructor",
                              PropertyDescriptor::data(function_ns, true, false, true));
    (void)set_own_property(realm.array_prototype_, "push", native_function_in_realm(realm, "push", 1, builtin_push));
    (void)set_own_property(realm.array_prototype_, "pop", native_function_in_realm(realm, "pop", 0, builtin_pop));
    (void)set_own_property(realm.array_prototype_, PropertyKey::symbol(runtime_->well_known_symbol("iterator").as_symbol_id()), native_function_in_realm(realm, "[Symbol.iterator]", 0, builtin_array_iterator));
    (void)set_own_property(realm.promise_prototype_, "then", native_function_in_realm(realm, "then", 1, builtin_promise_then));
    (void)set_own_property(realm.promise_prototype_, "catch", native_function_in_realm(realm, "catch", 1, builtin_promise_catch));
    (void)set_own_property(realm.regexp_prototype_, "test", native_function_in_realm(realm, "test", 1, builtin_regexp_test));
    (void)set_own_property(realm.regexp_prototype_, "exec", native_function_in_realm(realm, "exec", 1, builtin_regexp_exec));
    Value error_ns = native_function_in_realm(realm, "Error", 1, builtin_error_constructor, ConstructorKind::Base);
    Value type_error_ns = native_function_in_realm(realm, "TypeError", 1, builtin_type_error_constructor, ConstructorKind::Base);
    Value reference_error_ns = native_function_in_realm(realm, "ReferenceError", 1, builtin_reference_error_constructor, ConstructorKind::Base);
    auto wire_error_constructor = [&](Value constructor, Value prototype, std::string_view name) {
        (void)define_own_property(constructor, "prototype", PropertyDescriptor::data(prototype, false, false, false));
        (void)define_own_property(prototype, "constructor", PropertyDescriptor::data(constructor, true, false, true));
        (void)define_own_property(prototype, "name", PropertyDescriptor::data(string(name), true, false, true));
        (void)define_own_property(prototype, "message", PropertyDescriptor::data(string(""), true, false, true));
    };
    wire_error_constructor(error_ns, realm.error_prototype_, "Error");
    wire_error_constructor(type_error_ns, realm.type_error_prototype_, "TypeError");
    wire_error_constructor(reference_error_ns, realm.reference_error_prototype_, "ReferenceError");
    Value array_ns = native_function_in_realm(realm, "Array", 1, builtin_array_constructor, ConstructorKind::Base);
    (void)define_own_property(array_ns, "prototype", PropertyDescriptor::data(realm.array_prototype_, false, false, false));
    (void)define_own_property(realm.array_prototype_, "constructor", PropertyDescriptor::data(array_ns, true, false, true));
    (void)set_own_property(array_ns, "isArray", native_function_in_realm(realm, "isArray", 1, builtin_is_array));
    Value string_ns = native_function_in_realm(realm, "String", 1, builtin_string_constructor, ConstructorKind::None);
    Value number_ns=object_in_realm(realm); (void)set_own_property(number_ns,"isFinite",native_function_in_realm(realm,"isFinite",1,builtin_is_finite));
    Value object_ns=object_in_realm(realm); (void)set_own_property(object_ns,"getPrototypeOf",native_function_in_realm(realm,"getPrototypeOf",1,builtin_get_proto));
    (void)set_own_property(object_ns,"defineProperty",native_function_in_realm(realm,"defineProperty",3,builtin_object_define_property));
    (void)set_own_property(object_ns, "getOwnPropertyDescriptor", native_function_in_realm(realm, "getOwnPropertyDescriptor", 2, builtin_object_get_own_property_descriptor));
    (void)set_own_property(object_ns, "getOwnPropertyNames", native_function_in_realm(realm, "getOwnPropertyNames", 1, builtin_object_get_own_property_names));
    (void)set_own_property(object_ns,"keys",native_function_in_realm(realm,"keys",1,builtin_object_keys));
    (void)set_own_property(object_ns,"is",native_function_in_realm(realm,"is",2,builtin_object_is));
    (void)set_own_property(object_ns, "prototype", realm.object_prototype_);
    Value math_ns=object_in_realm(realm); (void)set_own_property(math_ns, "pow", native_function_in_realm(realm, "pow", 2, builtin_math_pow));
    Value promise_ns=object_in_realm(realm); (void)set_own_property(promise_ns,"resolve",native_function_in_realm(realm,"resolve",1,builtin_promise_resolve)); (void)set_own_property(promise_ns,"reject",native_function_in_realm(realm,"reject",1,builtin_promise_reject));
    Value symbol_ns = native_function_in_realm(realm, "Symbol", 0, builtin_symbol);
    (void)define_own_property(symbol_ns, "prototype", PropertyDescriptor::data(realm.symbol_prototype_, false, false, false));
    (void)define_own_property(realm.symbol_prototype_, "constructor", PropertyDescriptor::data(symbol_ns, true, false, true));
    (void)set_own_property(realm.symbol_prototype_, "toString", native_function_in_realm(realm, "toString", 0, builtin_symbol_to_string));
    (void)set_own_property(symbol_ns, "for", native_function_in_realm(realm, "for", 1, builtin_symbol_for));
    (void)set_own_property(symbol_ns, "keyFor", native_function_in_realm(realm, "keyFor", 1, builtin_symbol_key_for));
    for (const std::string_view name : {"iterator", "asyncIterator", "toPrimitive", "toStringTag", "hasInstance", "species", "match", "matchAll", "replace", "search", "split"}) {
        (void)set_own_property(symbol_ns, name, runtime_->well_known_symbol(name));
    }
    auto install_global = [&](std::string name, Value value) {
        (void)realm.global_environment_.create_global_var_binding(name, value);
        (void)set_own_property(realm.global_object_, name, value);
    };
    auto install_constant_global = [&](std::string name, Value value) {
        (void)realm.global_environment_.create_global_constant_binding(name, value);
        (void)define_own_property(realm.global_object_, name, PropertyDescriptor::data(value, false, false, false));
    };
    install_global("Function", function_ns);
    install_global("Error", error_ns);
    install_global("TypeError", type_error_ns);
    install_global("ReferenceError", reference_error_ns);
    install_global("Array", array_ns);
    install_global("Number", number_ns);
    install_global("String", string_ns);
    install_global("Object", object_ns);
    install_global("Math", math_ns);
    install_global("Promise", promise_ns);
    install_global("Symbol", symbol_ns);
    install_global("isNaN", native_function_in_realm(realm,"isNaN",1,builtin_is_nan));
    install_constant_global("Infinity", Value::number(std::numeric_limits<double>::infinity()));
    install_constant_global("NaN", Value::number(std::numeric_limits<double>::quiet_NaN()));
    install_constant_global("undefined", Value::undefined());
}

Value Context::array() { return array_in_realm(active_realm()); }
Value Context::array_in_realm(Realm& realm) { ensure_builtins(realm); Value v=runtime_->make_array(realm); v.as_heap_object()->prototype=realm.array_prototype_; return v; }
bool Context::is_array(const Value& v) const noexcept { return v.is_object() && v.as_heap_object()->is_array; }
bool Context::is_promise(const Value& v) const noexcept { return v.is_object() && v.as_heap_object()->is_promise; }

Result<Value> Context::promise_resolve(Value value) {
    const auto validation = validate(value); if (!validation) return validation.error();
    ensure_builtins();
    if (is_promise(value)) return value;
    Realm& realm = active_realm();
    Value promise = runtime_->make_promise(realm);
    promise.as_heap_object()->prototype = realm.promise_prototype_;
    const auto settled = runtime_->settle_promise(promise, true, value); if (!settled) return settled.error();
    return promise;
}

Result<Value> Context::promise_reject(Value value) {
    const auto validation = validate(value); if (!validation) return validation.error();
    ensure_builtins();
    Realm& realm = active_realm();
    Value promise = runtime_->make_promise(realm);
    promise.as_heap_object()->prototype = realm.promise_prototype_;
    const auto settled = runtime_->settle_promise(promise, false, value); if (!settled) return settled.error();
    return promise;
}

Result<Value> Context::promise_then(Value promise, Value on_fulfilled) {
    const auto pv = validate(promise); if (!pv) return pv.error();
    const auto hv = validate(on_fulfilled); if (!hv) return hv.error();
    if (!is_promise(promise)) return Error{ErrorCode::type_error, "Promise.prototype.then receiver is not a Promise"};
    if (!on_fulfilled.is_function()) return Error{ErrorCode::type_error, "Promise.prototype.then handler is not a function"};
    ensure_builtins();
    Realm& realm = promise.as_heap_object()->realm == nullptr ? active_realm() : *promise.as_heap_object()->realm;
    ensure_builtins(realm);
    Value next = runtime_->make_promise(realm); next.as_heap_object()->prototype = realm.promise_prototype_;
    auto* source = promise.as_heap_object();
    if (source->promise_state == detail::PromiseState::pending) {
        source->promise_reactions.push_back(detail::PromiseReaction{on_fulfilled, Value::undefined(), next});
    } else {
        runtime_->enqueue_promise_reaction(source->promise_state == detail::PromiseState::fulfilled, source->promise_result, on_fulfilled, Value::undefined(), next);
    }
    return next;
}

Result<Value> Context::promise_catch(Value promise, Value on_rejected) {
    const auto pv = validate(promise); if (!pv) return pv.error();
    const auto hv = validate(on_rejected); if (!hv) return hv.error();
    if (!is_promise(promise)) return Error{ErrorCode::type_error, "Promise.prototype.catch receiver is not a Promise"};
    if (!on_rejected.is_function()) return Error{ErrorCode::type_error, "Promise.prototype.catch handler is not a function"};
    ensure_builtins();
    Realm& realm = promise.as_heap_object()->realm == nullptr ? active_realm() : *promise.as_heap_object()->realm;
    ensure_builtins(realm);
    Value next = runtime_->make_promise(realm); next.as_heap_object()->prototype = realm.promise_prototype_;
    auto* source = promise.as_heap_object();
    if (source->promise_state == detail::PromiseState::pending) {
        source->promise_reactions.push_back(detail::PromiseReaction{Value::undefined(), on_rejected, next});
    } else {
        runtime_->enqueue_promise_reaction(source->promise_state == detail::PromiseState::fulfilled, source->promise_result, Value::undefined(), on_rejected, next);
    }
    return next;
}

Result<Value> Context::promise_result(Value promise) const {
    const auto validation = validate(promise); if (!validation) return validation.error();
    if (!is_promise(promise)) return Error{ErrorCode::type_error, "value is not a Promise"};
    const auto* p = promise.as_heap_object();
    if (p->promise_state == detail::PromiseState::pending) return Error{ErrorCode::internal, "Promise is still pending"};
    return p->promise_result;
}
Result<Value> Context::get_global(std::string_view name) { ensure_builtins(); return active_realm().global_environment_.get_binding_value(name); }
Result<Value> Context::array_push(Value array_value, Value value) const {
    if (!is_array(array_value)) return Error{ErrorCode::type_error, "Array.prototype.push receiver is not an array"};
    auto* array = array_value.as_heap_object();
    if (array->array_length == 0xFFFFFFFFU) return Error{ErrorCode::type_error, "array length overflow"};
    const PropertyKey key = PropertyKey::atom(runtime_->intern_atom(std::to_string(array->array_length)));
    const auto defined = define_own_property(array_value, key, PropertyDescriptor::data(value, true, true, true));
    if (!defined) return defined.error();
    if (!*defined) return Error{ErrorCode::type_error, "array index or length is not writable"};
    return Value::number(static_cast<double>(array->array_length));
}
Result<Value> Context::array_pop(Value array_value) const {
    if (!is_array(array_value)) return Error{ErrorCode::type_error, "Array.prototype.pop receiver is not an array"};
    auto* array = array_value.as_heap_object();
    if (array->array_length == 0) return Value::undefined();
    const std::uint32_t index = array->array_length - 1U;
    const PropertyKey key = PropertyKey::atom(runtime_->intern_atom(std::to_string(index)));
    const auto value = const_cast<Context*>(this)->get_property(array_value, key); if (!value) return value.error();
    const auto deleted = delete_property(array_value, key); if (!deleted) return deleted.error();
    if (!*deleted) return Error{ErrorCode::type_error, "array element is not configurable"};
    PropertyDescriptor length; length.value = Value::number(static_cast<double>(index));
    const auto shrunk = define_own_property(array_value, PropertyKey::atom(runtime_->intern_atom("length")), length);
    if (!shrunk) return shrunk.error();
    if (!*shrunk) return Error{ErrorCode::type_error, "array length is not writable"};
    return *value;
}
Result<Value> Context::array_iterator(Value array_value) {
    if (!array_value.is_object_like()) return Error{ErrorCode::type_error, "iterator receiver is not an object"};
    Value iterator = object();
    auto a = set_own_property(iterator, "_array", array_value); if (!a) return a.error();
    auto i = set_own_property(iterator, "_index", number(0)); if (!i) return i.error();
    auto n = set_own_property(iterator, "next", native_function("next", 0, builtin_array_iterator_next)); if (!n) return n.error();
    return iterator;
}

Result<Value> Context::array_iterator_next(Value iterator) {
    if (!iterator.is_object()) return Error{ErrorCode::type_error, "array iterator next receiver is not an object"};
    const auto array_value = get_own_property(iterator, "_array"); if (!array_value) return array_value.error();
    const auto index_value = get_own_property(iterator, "_index"); if (!index_value) return index_value.error();
    if (!array_value->is_object_like() || !index_value->is_number()) return Error{ErrorCode::type_error, "invalid array iterator state"};

    const double index_number = index_value->as_number();
    if (index_number < 0 || !std::isfinite(index_number) || std::floor(index_number) != index_number) return Error{ErrorCode::internal, "invalid array iterator index"};
    const auto index = static_cast<std::uint32_t>(index_number);
    const auto length_value = get_property(*array_value, "length"); if (!length_value) return length_value.error();
    const auto length_result = abstract_operations::to_length(*this, *length_value);
    if (!length_result) return length_result.error().legacy_error();
    if (!length_result.completion().is_normal()) return Error{ErrorCode::uncaught_exception, "abrupt completion while reading iterator length"};
    const auto length = static_cast<std::uint32_t>(length_result.completion().value().as_number());

    Value result = object();
    if (index >= length) {
        auto done = set_own_property(result, "done", boolean(true)); if (!done) return done.error();
        auto value = set_own_property(result, "value", undefined()); if (!value) return value.error();
        return result;
    }

    const PropertyKey element_key = PropertyKey::atom(runtime_->intern_atom(std::to_string(index)));
    const auto element = get_property(*array_value, element_key); if (!element) return element.error();
    auto done = set_own_property(result, "done", boolean(false)); if (!done) return done.error();
    auto value = set_own_property(result, "value", *element); if (!value) return value.error();
    auto advance = set_own_property(iterator, "_index", number(index_number + 1.0)); if (!advance) return advance.error();
    return result;
}
ExecutionResult Context::get_element_semantic(const Value& object, Value key) {
    const auto object_validation = validate(object); if (!object_validation) return object_validation.error();
    const auto key_validation = validate(key); if (!key_validation) return key_validation.error();
    if (!object.is_object_like()) return Error{ErrorCode::type_error, "element access target is not an object"};

    const auto converted_key = abstract_operations::to_property_key(*this, key);
    if (!converted_key) return converted_key.error();
    if (!converted_key.completion().is_normal()) return converted_key.completion();
    const auto property_key_result = property_key(converted_key.completion().value());
    if (!property_key_result) return property_key_result.error();
    return get_property_semantic(object, *property_key_result, object);
}

Result<Value> Context::get_element(const Value& object, Value key) {
    const ExecutionResult result = get_element_semantic(object, key);
    if (!result) return result.error().legacy_error();
    if (result.completion().is_throw()) return Error{ErrorCode::uncaught_exception, "uncaught JavaScript exception: " + result.completion().value().to_debug_string()};
    if (!result.completion().is_normal()) return Error{ErrorCode::internal, "non-normal completion escaped element get"};
    return result.completion().value();
}

ExecutionResult Context::set_element_semantic(const Value& object, Value key, Value value) {
    const auto object_validation = validate(object); if (!object_validation) return object_validation.error();
    const auto key_validation = validate(key); if (!key_validation) return key_validation.error();
    const auto value_validation = validate(value); if (!value_validation) return value_validation.error();
    if (!object.is_object_like()) return Error{ErrorCode::type_error, "element write target is not an object"};

    const auto converted_key = abstract_operations::to_property_key(*this, key);
    if (!converted_key) return converted_key.error();
    if (!converted_key.completion().is_normal()) return converted_key.completion();
    const auto property_key_result = property_key(converted_key.completion().value());
    if (!property_key_result) return property_key_result.error();
    return set_property_semantic(object, *property_key_result, value, object);
}

Result<void> Context::set_element(const Value& object, Value key, Value value) {
    const ExecutionResult result = set_element_semantic(object, key, value);
    if (!result) return result.error().legacy_error();
    if (result.completion().is_throw()) return Error{ErrorCode::uncaught_exception, "uncaught JavaScript exception: " + result.completion().value().to_debug_string()};
    if (!result.completion().is_normal()) return Error{ErrorCode::internal, "non-normal completion escaped element set"};
    if (!result.completion().value().is_boolean() || !result.completion().value().as_boolean()) return Error{ErrorCode::type_error, "property is not writable"};
    return {};
}


} // namespace js
