#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <js/completion.hpp>
#include <js/property_descriptor.hpp>
#include <js/property_key.hpp>
#include <js/result.hpp>
#include <js/value.hpp>

namespace js {
class Context;

enum class PrimitiveHint : std::uint8_t {
    Default,
    Number,
    String,
};

struct IteratorRecord final {
    Value iterator{Value::undefined()};
    Value next_method{Value::undefined()};
};

namespace abstract_operations {

[[nodiscard]] bool to_boolean(Value value) noexcept;

// Completion-aware semantic conversion operations. A JavaScript exception raised by
// user coercion remains CompletionType::Throw; EngineFailure is reserved for engine/host faults.
[[nodiscard]] ExecutionResult to_primitive(Context& context, Value value, PrimitiveHint hint = PrimitiveHint::Default);
[[nodiscard]] ExecutionResult ordinary_to_primitive(Context& context, Value object, PrimitiveHint hint);
[[nodiscard]] ExecutionResult to_numeric(Context& context, Value value);
[[nodiscard]] ExecutionResult to_number(Context& context, Value value);
[[nodiscard]] ExecutionResult to_integer_or_infinity(Context& context, Value value);
[[nodiscard]] ExecutionResult to_int32(Context& context, Value value);
[[nodiscard]] ExecutionResult to_uint32(Context& context, Value value);
[[nodiscard]] ExecutionResult to_uint8_clamp(Context& context, Value value);
[[nodiscard]] ExecutionResult to_string(Context& context, Value value);
[[nodiscard]] ExecutionResult to_object(Context& context, Value value);
[[nodiscard]] ExecutionResult to_property_key(Context& context, Value value);
[[nodiscard]] ExecutionResult to_length(Context& context, Value value);
[[nodiscard]] ExecutionResult to_index(Context& context, Value value);

// ECMAScript Number::exponentiate for the currently-supported Number numeric domain.
[[nodiscard]] double number_exponentiate(double base, double exponent) noexcept;

[[nodiscard]] bool is_callable(Value value) noexcept;
[[nodiscard]] bool is_constructor(Value value) noexcept;
[[nodiscard]] bool is_array(Context& context, Value value) noexcept;
[[nodiscard]] bool is_regexp(Context& context, Value value) noexcept;

[[nodiscard]] Result<bool> same_value(Context& context, Value left, Value right);
[[nodiscard]] Result<bool> same_value_zero(Context& context, Value left, Value right);
[[nodiscard]] Result<bool> strict_equal(Context& context, Value left, Value right);
[[nodiscard]] ExecutionResult abstract_equal(Context& context, Value left, Value right);
// Normal(undefined) represents the ECMAScript undefined result of Abstract Relational Comparison.
[[nodiscard]] ExecutionResult abstract_relational_compare(Context& context, Value left, Value right);
[[nodiscard]] ExecutionResult instanceof_operator(Context& context, Value object, Value constructor);

[[nodiscard]] ExecutionResult get(Context& context, Value object, PropertyKey key, Value receiver);
[[nodiscard]] ExecutionResult get(Context& context, Value object, PropertyKey key);
[[nodiscard]] ExecutionResult set(Context& context, Value object, PropertyKey key, Value value, Value receiver);
[[nodiscard]] ExecutionResult set(Context& context, Value object, PropertyKey key, Value value);
[[nodiscard]] Result<bool> has_property(Context& context, Value object, PropertyKey key);
[[nodiscard]] Result<bool> has_own_property(Context& context, Value object, PropertyKey key);
[[nodiscard]] Result<bool> create_data_property(Context& context, Value object, PropertyKey key, Value value);
[[nodiscard]] Result<void> define_property_or_throw(Context& context, Value object, PropertyKey key, const PropertyDescriptor& descriptor);
[[nodiscard]] Result<void> delete_property_or_throw(Context& context, Value object, PropertyKey key);
[[nodiscard]] ExecutionResult get_method(Context& context, Value object, PropertyKey key);
[[nodiscard]] ExecutionResult call(Context& context, Value callable, Value this_value, std::span<const Value> arguments = {});
[[nodiscard]] ExecutionResult construct(Context& context, Value constructor, std::span<const Value> arguments = {});

// Iterator protocol scaffolding. P12 completes iterator-close and async/generator integration,
// but all later consumers use these stable interfaces.
[[nodiscard]] ExecutionResult get_iterator(Context& context, Value value);
[[nodiscard]] ExecutionResult iterator_next(Context& context, const IteratorRecord& iterator, std::optional<Value> value = std::nullopt);
[[nodiscard]] ExecutionResult iterator_complete(Context& context, Value iterator_result);
[[nodiscard]] ExecutionResult iterator_value(Context& context, Value iterator_result);
[[nodiscard]] ExecutionResult iterator_close(Context& context, const IteratorRecord& iterator, Completion completion);
[[nodiscard]] ExecutionResult create_iter_result_object(Context& context, Value value, bool done);

[[nodiscard]] ExecutionResult species_constructor(Context& context, Value object, Value default_constructor);
[[nodiscard]] ExecutionResult create_array_from_list(Context& context, std::span<const Value> values);
[[nodiscard]] ExecutionResult create_list_from_array_like(Context& context, Value value);

} // namespace abstract_operations
} // namespace js
