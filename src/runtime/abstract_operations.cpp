#include <js/abstract_operations.hpp>

#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include <js/context.hpp>
#include <js/error.hpp>


namespace js::abstract_operations {
namespace {

[[nodiscard]] std::string_view trim_ascii_whitespace(std::string_view text) noexcept {
    auto is_space = [](char c) noexcept {
        switch (c) {
        case ' ': case '\t': case '\n': case '\r': case '\f': case '\v': return true;
        default: return false;
        }
    };
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return text;
}

[[nodiscard]] double string_to_number(std::string_view source) {
    const std::string_view text = trim_ascii_whitespace(source);
    if (text.empty()) return 0.0;
    if (text == "Infinity" || text == "+Infinity") return std::numeric_limits<double>::infinity();
    if (text == "-Infinity") return -std::numeric_limits<double>::infinity();

    const std::string_view parse_text = (!text.empty() && text.front() == '+') ? text.substr(1) : text;
    if (parse_text.empty()) return std::numeric_limits<double>::quiet_NaN();

    if (parse_text.size() > 2U && parse_text[0] == '0' &&
        (parse_text[1] == 'x' || parse_text[1] == 'X' ||
         parse_text[1] == 'b' || parse_text[1] == 'B' ||
         parse_text[1] == 'o' || parse_text[1] == 'O')) {
        int base = 10;
        if (parse_text[1] == 'x' || parse_text[1] == 'X') base = 16;
        else if (parse_text[1] == 'b' || parse_text[1] == 'B') base = 2;
        else base = 8;
        const std::string_view digits = parse_text.substr(2);
        unsigned long long integer = 0;
        const auto converted = std::from_chars(digits.data(), digits.data() + digits.size(), integer, base);
        if (converted.ec != std::errc{} || converted.ptr != digits.data() + digits.size())
            return std::numeric_limits<double>::quiet_NaN();
        return static_cast<double>(integer);
    }

    double value = 0.0;
    const auto parsed = std::from_chars(parse_text.data(), parse_text.data() + parse_text.size(), value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != parse_text.data() + parse_text.size())
        return std::numeric_limits<double>::quiet_NaN();
    return value;
}

[[nodiscard]] std::string number_to_string(double number) {
    if (std::isnan(number)) return "NaN";
    if (std::isinf(number)) return number < 0.0 ? "-Infinity" : "Infinity";
    if (number == 0.0) return "0";
    char buffer[128]{};
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), number, std::chars_format::general);
    if (converted.ec == std::errc{}) return std::string(buffer, converted.ptr);
    return "NaN";
}

[[nodiscard]] ExecutionResult type_error(Context& context, std::string_view message) {
    // Error objects are completed in P15; the important substrate invariant here is that
    // language-level failure travels through Completion::Throw, never EngineFailure.
    return Completion::throw_(context.string(std::string("TypeError: ") + std::string(message)));
}

[[nodiscard]] bool same_type_strict_equal(Value left, Value right) noexcept {
    switch (left.tag()) {
    case ValueTag::undefined:
    case ValueTag::null: return true;
    case ValueTag::boolean: return left.as_boolean() == right.as_boolean();
    case ValueTag::number: {
        const double lhs = left.as_number();
        const double rhs = right.as_number();
        return !std::isnan(lhs) && !std::isnan(rhs) && lhs == rhs;
    }
    case ValueTag::string: return left.as_string() == right.as_string();
    case ValueTag::symbol: return left.as_symbol_id() == right.as_symbol_id();
    case ValueTag::function:
    case ValueTag::object: return left.same_heap_identity(right);
    }
    return false;
}

[[nodiscard]] bool same_value_impl(Value left, Value right, bool zero_equal) noexcept {
    if (left.tag() != right.tag()) return false;
    if (left.is_number()) {
        const double lhs = left.as_number();
        const double rhs = right.as_number();
        if (std::isnan(lhs) && std::isnan(rhs)) return true;
        if (!zero_equal && lhs == 0.0 && rhs == 0.0) return std::signbit(lhs) == std::signbit(rhs);
        return lhs == rhs;
    }
    return same_type_strict_equal(left, right);
}

[[nodiscard]] std::string_view hint_name(PrimitiveHint hint) noexcept {
    switch (hint) {
    case PrimitiveHint::Default: return "default";
    case PrimitiveHint::Number: return "number";
    case PrimitiveHint::String: return "string";
    }
    return "default";
}


} // namespace

bool to_boolean(Value value) noexcept {
    switch (value.tag()) {
    case ValueTag::undefined:
    case ValueTag::null: return false;
    case ValueTag::boolean: return value.as_boolean();
    case ValueTag::number: return value.as_number() != 0.0 && !std::isnan(value.as_number());
    case ValueTag::string: return !value.as_string().empty();
    case ValueTag::symbol:
    case ValueTag::function:
    case ValueTag::object: return true;
    }
    return false;
}

ExecutionResult ordinary_to_primitive(Context& context, Value object, PrimitiveHint hint) {
    const auto validation = context.validate(object);
    if (!validation) return validation.error();
    if (!object.is_object_like()) return Completion::normal(object);

    const bool string_first = hint == PrimitiveHint::String;
    const std::string_view first = string_first ? "toString" : "valueOf";
    const std::string_view second = string_first ? "valueOf" : "toString";
    for (const std::string_view name : {first, second}) {
        const ExecutionResult method_result = get(context, object, context.property_key(name));
        if (!method_result) return method_result.error();
        if (!method_result.completion().is_normal()) return method_result.completion();
        const Value method = method_result.completion().value();
        if (!is_callable(method)) continue;
        const ExecutionResult result = call(context, method, object);
        if (!result) return result.error();
        if (!result.completion().is_normal()) return result.completion();
        if (!result.completion().value().is_object_like()) return result.completion();
    }
    return type_error(context, "cannot convert object to primitive value");
}

ExecutionResult to_primitive(Context& context, Value value, PrimitiveHint hint) {
    const auto validation = context.validate(value);
    if (!validation) return validation.error();
    if (!value.is_object_like()) return Completion::normal(value);

    const PropertyKey exotic_key = context.property_key(context.well_known_symbol("toPrimitive")).value();
    const ExecutionResult exotic_result = get_method(context, value, exotic_key);
    if (!exotic_result) return exotic_result.error();
    if (!exotic_result.completion().is_normal()) return exotic_result.completion();
    const Value exotic = exotic_result.completion().value();
    if (!exotic.is_undefined()) {
        const Value hint_value = context.string(hint_name(hint));
        const ExecutionResult result = call(context, exotic, value, std::span<const Value>(&hint_value, 1U));
        if (!result) return result.error();
        if (!result.completion().is_normal()) return result.completion();
        if (result.completion().value().is_object_like())
            return type_error(context, "@@toPrimitive must return a primitive value");
        return result.completion();
    }
    return ordinary_to_primitive(context, value, hint == PrimitiveHint::Default ? PrimitiveHint::Number : hint);
}

ExecutionResult to_numeric(Context& context, Value value) {
    // The interface is deliberately Value-based: once BigInt becomes a Value primitive in P15,
    // this operation can return either Number or BigInt without another signature redesign.
    return to_number(context, value);
}

ExecutionResult to_number(Context& context, Value value) {
    const ExecutionResult primitive_result = to_primitive(context, value, PrimitiveHint::Number);
    if (!primitive_result) return primitive_result.error();
    if (!primitive_result.completion().is_normal()) return primitive_result.completion();
    const Value primitive = primitive_result.completion().value();
    switch (primitive.tag()) {
    case ValueTag::undefined: return Completion::normal(Value::number(std::numeric_limits<double>::quiet_NaN()));
    case ValueTag::null: return Completion::normal(Value::number(0.0));
    case ValueTag::boolean: return Completion::normal(Value::number(primitive.as_boolean() ? 1.0 : 0.0));
    case ValueTag::number: return Completion::normal(primitive);
    case ValueTag::string: return Completion::normal(Value::number(string_to_number(primitive.as_string())));
    case ValueTag::symbol: return type_error(context, "cannot convert Symbol to number");
    case ValueTag::function:
    case ValueTag::object: return EngineFailure{EngineFailureCode::InternalInvariant, "ToNumber received object after ToPrimitive"};
    }
    return EngineFailure{EngineFailureCode::InternalInvariant, "unknown Value tag in ToNumber"};
}

ExecutionResult to_integer_or_infinity(Context& context, Value value) {
    const ExecutionResult number_result = to_number(context, value);
    if (!number_result || !number_result.completion().is_normal()) return number_result;
    const double number = number_result.completion().value().as_number();
    if (std::isnan(number) || number == 0.0) return Completion::normal(Value::number(number == 0.0 ? number : 0.0));
    if (std::isinf(number)) return number_result.completion();
    return Completion::normal(Value::number(std::trunc(number)));
}

ExecutionResult to_uint32(Context& context, Value value) {
    const ExecutionResult number_result = to_number(context, value);
    if (!number_result || !number_result.completion().is_normal()) return number_result;
    const double number = number_result.completion().value().as_number();
    if (!std::isfinite(number) || number == 0.0) return Completion::normal(Value::number(0.0));
    constexpr double modulo = 4294967296.0;
    double wrapped = std::fmod(std::trunc(number), modulo);
    if (wrapped < 0.0) wrapped += modulo;
    return Completion::normal(Value::number(wrapped));
}

ExecutionResult to_int32(Context& context, Value value) {
    const ExecutionResult uint_result = to_uint32(context, value);
    if (!uint_result || !uint_result.completion().is_normal()) return uint_result;
    const auto bits = static_cast<std::uint32_t>(uint_result.completion().value().as_number());
    return Completion::normal(Value::number(static_cast<double>(std::bit_cast<std::int32_t>(bits))));
}

ExecutionResult to_uint8_clamp(Context& context, Value value) {
    const ExecutionResult number_result = to_number(context, value);
    if (!number_result || !number_result.completion().is_normal()) return number_result;
    const double number = number_result.completion().value().as_number();
    if (std::isnan(number) || number <= 0.0) return Completion::normal(Value::number(0.0));
    if (number >= 255.0) return Completion::normal(Value::number(255.0));
    const double floor_value = std::floor(number);
    double result = floor_value;
    const double fraction = number - floor_value;
    if (fraction > 0.5 || (fraction == 0.5 && std::fmod(floor_value, 2.0) != 0.0)) result += 1.0;
    return Completion::normal(Value::number(result));
}

ExecutionResult to_string(Context& context, Value value) {
    const ExecutionResult primitive_result = to_primitive(context, value, PrimitiveHint::String);
    if (!primitive_result || !primitive_result.completion().is_normal()) return primitive_result;
    const Value primitive = primitive_result.completion().value();
    switch (primitive.tag()) {
    case ValueTag::undefined: return Completion::normal(context.string("undefined"));
    case ValueTag::null: return Completion::normal(context.string("null"));
    case ValueTag::boolean: return Completion::normal(context.string(primitive.as_boolean() ? "true" : "false"));
    case ValueTag::number: return Completion::normal(context.string(number_to_string(primitive.as_number())));
    case ValueTag::string: return Completion::normal(primitive);
    case ValueTag::symbol: return type_error(context, "cannot convert Symbol to string");
    case ValueTag::function:
    case ValueTag::object: return EngineFailure{EngineFailureCode::InternalInvariant, "ToString received object after ToPrimitive"};
    }
    return EngineFailure{EngineFailureCode::InternalInvariant, "unknown Value tag in ToString"};
}

ExecutionResult to_object(Context& context, Value value) {
    const auto validation = context.validate(value);
    if (!validation) return validation.error();
    if (value.is_undefined() || value.is_null()) return type_error(context, "cannot convert null or undefined to object");
    if (value.is_object_like()) return Completion::normal(value);
    const Value boxed = context.box_primitive(value);
    if (boxed.is_undefined()) return EngineFailure{EngineFailureCode::InternalInvariant, "failed to box primitive"};
    return Completion::normal(boxed);
}

ExecutionResult to_property_key(Context& context, Value value) {
    const ExecutionResult key_result = to_primitive(context, value, PrimitiveHint::String);
    if (!key_result || !key_result.completion().is_normal()) return key_result;
    const Value key = key_result.completion().value();
    if (key.is_symbol()) return key_result.completion();
    return to_string(context, key);
}

ExecutionResult to_length(Context& context, Value value) {
    const ExecutionResult integer_result = to_integer_or_infinity(context, value);
    if (!integer_result || !integer_result.completion().is_normal()) return integer_result;
    double length = integer_result.completion().value().as_number();
    if (length <= 0.0) length = 0.0;
    constexpr double max_safe_integer = 9007199254740991.0;
    if (length > max_safe_integer) length = max_safe_integer;
    return Completion::normal(Value::number(length));
}

ExecutionResult to_index(Context& context, Value value) {
    if (value.is_undefined()) return Completion::normal(Value::number(0.0));
    const ExecutionResult integer_result = to_integer_or_infinity(context, value);
    if (!integer_result || !integer_result.completion().is_normal()) return integer_result;
    const double integer = integer_result.completion().value().as_number();
    if (integer < 0.0 || !std::isfinite(integer) || integer > 9007199254740991.0)
        return type_error(context, "index is outside the valid range");
    return Completion::normal(Value::number(integer));
}

bool is_callable(Value value) noexcept { return value.is_function(); }
bool is_constructor(Value value) noexcept { return value.is_constructor(); }
bool is_array(Context& context, Value value) noexcept { return context.is_array(value); }
bool is_regexp(Context& context, Value value) noexcept { return context.is_regexp(value); }

Result<bool> same_value(Context& context, Value left, Value right) {
    const auto l = context.validate(left); if (!l) return l.error();
    const auto r = context.validate(right); if (!r) return r.error();
    return same_value_impl(left, right, false);
}

Result<bool> same_value_zero(Context& context, Value left, Value right) {
    const auto l = context.validate(left); if (!l) return l.error();
    const auto r = context.validate(right); if (!r) return r.error();
    return same_value_impl(left, right, true);
}

Result<bool> strict_equal(Context& context, Value left, Value right) {
    const auto l = context.validate(left); if (!l) return l.error();
    const auto r = context.validate(right); if (!r) return r.error();
    if (left.tag() != right.tag()) return false;
    return same_type_strict_equal(left, right);
}

ExecutionResult abstract_equal(Context& context, Value left, Value right) {
    const auto l = context.validate(left); if (!l) return l.error();
    const auto r = context.validate(right); if (!r) return r.error();
    if (left.tag() == right.tag()) return Completion::normal(Value::boolean(same_type_strict_equal(left, right)));
    if ((left.is_null() && right.is_undefined()) || (left.is_undefined() && right.is_null()))
        return Completion::normal(Value::boolean(true));

    if (left.is_number() && right.is_string()) {
        const ExecutionResult rhs = to_number(context, right);
        if (!rhs || !rhs.completion().is_normal()) return rhs;
        const double rv = rhs.completion().value().as_number();
        return Completion::normal(Value::boolean(!std::isnan(left.as_number()) && !std::isnan(rv) && left.as_number() == rv));
    }
    if (left.is_string() && right.is_number()) return abstract_equal(context, right, left);
    if (left.is_boolean()) return abstract_equal(context, Value::number(left.as_boolean() ? 1.0 : 0.0), right);
    if (right.is_boolean()) return abstract_equal(context, left, Value::number(right.as_boolean() ? 1.0 : 0.0));

    if (left.is_object_like() && !right.is_object_like()) {
        const ExecutionResult primitive = to_primitive(context, left);
        if (!primitive || !primitive.completion().is_normal()) return primitive;
        return abstract_equal(context, primitive.completion().value(), right);
    }
    if (!left.is_object_like() && right.is_object_like()) {
        const ExecutionResult primitive = to_primitive(context, right);
        if (!primitive || !primitive.completion().is_normal()) return primitive;
        return abstract_equal(context, left, primitive.completion().value());
    }
    return Completion::normal(Value::boolean(false));
}

ExecutionResult abstract_relational_compare(Context& context, Value left, Value right) {
    const ExecutionResult lhs_p = to_primitive(context, left, PrimitiveHint::Number);
    if (!lhs_p || !lhs_p.completion().is_normal()) return lhs_p;
    const ExecutionResult rhs_p = to_primitive(context, right, PrimitiveHint::Number);
    if (!rhs_p || !rhs_p.completion().is_normal()) return rhs_p;
    const Value lhs_primitive = lhs_p.completion().value();
    const Value rhs_primitive = rhs_p.completion().value();

    if (lhs_primitive.is_string() && rhs_primitive.is_string())
        return Completion::normal(Value::boolean(lhs_primitive.as_string() < rhs_primitive.as_string()));

    const ExecutionResult lhs = to_number(context, lhs_primitive);
    if (!lhs || !lhs.completion().is_normal()) return lhs;
    const ExecutionResult rhs = to_number(context, rhs_primitive);
    if (!rhs || !rhs.completion().is_normal()) return rhs;
    const double lnum = lhs.completion().value().as_number();
    const double rnum = rhs.completion().value().as_number();
    if (std::isnan(lnum) || std::isnan(rnum)) return Completion::normal(Value::undefined());
    return Completion::normal(Value::boolean(lnum < rnum));
}



ExecutionResult instanceof_operator(Context& context, Value object, Value constructor) {
    if (!constructor.is_object_like()) return type_error(context, "right-hand side of instanceof is not an object");

    const PropertyKey has_instance_key = context.property_key(context.well_known_symbol("hasInstance")).value();
    const ExecutionResult method = get_method(context, constructor, has_instance_key);
    if (!method) return method.error();
    if (!method.completion().is_normal()) return method.completion();
    if (!method.completion().value().is_undefined()) {
        const Value argument = object;
        const ExecutionResult result = call(context, method.completion().value(), constructor, std::span<const Value>(&argument, 1U));
        if (!result || !result.completion().is_normal()) return result;
        return Completion::normal(Value::boolean(to_boolean(result.completion().value())));
    }

    if (!is_callable(constructor)) return type_error(context, "right-hand side of instanceof is not callable");

    const auto bound_target = context.function_bound_target(constructor);
    if (!bound_target) return bound_target.error();
    if (!bound_target->is_undefined()) return instanceof_operator(context, object, *bound_target);
    if (!object.is_object_like()) return Completion::normal(Value::boolean(false));

    const ExecutionResult prototype_result = get(context, constructor, context.property_key("prototype"));
    if (!prototype_result || !prototype_result.completion().is_normal()) return prototype_result;
    const Value prototype = prototype_result.completion().value();
    if (!prototype.is_object_like()) return type_error(context, "function has non-object prototype in instanceof check");

    Value current = object;
    while (current.is_object_like()) {
        const auto next = context.get_prototype(current);
        if (!next) return next.error();
        current = *next;
        if (current.is_null()) return Completion::normal(Value::boolean(false));
        const auto same = same_value(context, current, prototype);
        if (!same) return same.error();
        if (*same) return Completion::normal(Value::boolean(true));
    }
    return Completion::normal(Value::boolean(false));
}

ExecutionResult get(Context& context, Value object, PropertyKey key, Value receiver) { return context.get_property_semantic(object, key, receiver); }
ExecutionResult get(Context& context, Value object, PropertyKey key) { return context.get_property_semantic(object, key, object); }
ExecutionResult set(Context& context, Value object, PropertyKey key, Value value, Value receiver) { return context.set_property_semantic(object, key, value, receiver); }
ExecutionResult set(Context& context, Value object, PropertyKey key, Value value) { return context.set_property_semantic(object, key, value, object); }
Result<bool> has_property(Context& context, Value object, PropertyKey key) { return context.has_property(object, key); }
Result<bool> has_own_property(Context& context, Value object, PropertyKey key) {
    const auto descriptor = context.get_own_property_descriptor(object, key);
    if (!descriptor) return descriptor.error();
    return descriptor->has_value();
}
Result<bool> create_data_property(Context& context, Value object, PropertyKey key, Value value) {
    return context.define_own_property(object, key, PropertyDescriptor::data(value, true, true, true));
}
Result<void> define_property_or_throw(Context& context, Value object, PropertyKey key, const PropertyDescriptor& descriptor) {
    const auto result = context.define_own_property(object, key, descriptor);
    if (!result) return result.error();
    if (!*result) return Error{ErrorCode::type_error, "DefinePropertyOrThrow rejected property definition"};
    return {};
}
Result<void> delete_property_or_throw(Context& context, Value object, PropertyKey key) {
    const auto result = context.delete_property(object, key);
    if (!result) return result.error();
    if (!*result) return Error{ErrorCode::type_error, "DeletePropertyOrThrow rejected non-configurable property"};
    return {};
}

ExecutionResult get_method(Context& context, Value object, PropertyKey key) {
    const ExecutionResult method = get(context, object, key);
    if (!method) return method.error();
    if (!method.completion().is_normal()) return method.completion();
    const Value value = method.completion().value();
    if (value.is_undefined() || value.is_null()) return Completion::normal(Value::undefined());
    if (!is_callable(value)) return type_error(context, "property is not callable");
    return Completion::normal(value);
}

ExecutionResult call(Context& context, Value callable, Value this_value, std::span<const Value> arguments) {
    if (!is_callable(callable)) return type_error(context, "value is not callable");
    return context.invoke(callable, arguments, this_value);
}

ExecutionResult construct(Context& context, Value constructor, std::span<const Value> arguments) {
    if (!is_constructor(constructor)) return type_error(context, "value is not a constructor");
    return context.construct(constructor, arguments);
}

ExecutionResult get_iterator(Context& context, Value value) {
    const ExecutionResult method = get_method(context, value, context.property_key(context.well_known_symbol("iterator")).value());
    if (!method || !method.completion().is_normal()) return method;
    if (method.completion().value().is_undefined()) return type_error(context, "value is not iterable");
    const ExecutionResult iterator = call(context, method.completion().value(), value);
    if (!iterator || !iterator.completion().is_normal()) return iterator;
    if (!iterator.completion().value().is_object_like()) return type_error(context, "iterator method did not return an object");
    return iterator;
}

ExecutionResult iterator_next(Context& context, const IteratorRecord& iterator, std::optional<Value> value) {
    if (!iterator.iterator.is_object_like() || !is_callable(iterator.next_method))
        return EngineFailure{EngineFailureCode::HostContractViolation, "invalid IteratorRecord"};
    ExecutionResult result = value
        ? call(context, iterator.next_method, iterator.iterator, std::span<const Value>(&*value, 1U))
        : call(context, iterator.next_method, iterator.iterator);
    if (!result || !result.completion().is_normal()) return result;
    if (!result.completion().value().is_object_like()) return type_error(context, "iterator next() did not return an object");
    return result;
}

ExecutionResult iterator_complete(Context& context, Value iterator_result) {
    const ExecutionResult done = get(context, iterator_result, context.property_key("done"));
    if (!done || !done.completion().is_normal()) return done;
    return Completion::normal(Value::boolean(to_boolean(done.completion().value())));
}

ExecutionResult iterator_value(Context& context, Value iterator_result) {
    return get(context, iterator_result, context.property_key("value"));
}

ExecutionResult iterator_close(Context& context, const IteratorRecord& iterator, Completion completion) {
    const ExecutionResult return_method = get_method(context, iterator.iterator, context.property_key("return"));
    if (!return_method) return return_method.error();
    if (!return_method.completion().is_normal()) return return_method.completion();
    if (return_method.completion().value().is_undefined()) return completion;
    const ExecutionResult inner = call(context, return_method.completion().value(), iterator.iterator);
    if (!inner) return inner.error();
    if (!inner.completion().is_normal()) return inner.completion();
    if (!inner.completion().value().is_object_like()) return type_error(context, "iterator return() did not return an object");
    return completion;
}

ExecutionResult create_iter_result_object(Context& context, Value value, bool done) {
    Value result = context.object();
    const auto v = create_data_property(context, result, context.property_key("value"), value);
    if (!v) return v.error();
    const auto d = create_data_property(context, result, context.property_key("done"), Value::boolean(done));
    if (!d) return d.error();
    return Completion::normal(result);
}

ExecutionResult species_constructor(Context& context, Value object, Value default_constructor) {
    if (!object.is_object_like()) return type_error(context, "SpeciesConstructor target is not an object");
    const ExecutionResult constructor = get(context, object, context.property_key("constructor"));
    if (!constructor || !constructor.completion().is_normal()) return constructor;
    const Value ctor = constructor.completion().value();
    if (ctor.is_undefined()) return Completion::normal(default_constructor);
    if (!ctor.is_object_like()) return type_error(context, "constructor property is not an object");
    const ExecutionResult species = get(context, ctor, context.property_key(context.well_known_symbol("species")).value());
    if (!species || !species.completion().is_normal()) return species;
    const Value candidate = species.completion().value();
    if (candidate.is_undefined() || candidate.is_null()) return Completion::normal(default_constructor);
    if (!is_constructor(candidate)) return type_error(context, "@@species is not a constructor");
    return Completion::normal(candidate);
}

ExecutionResult create_array_from_list(Context& context, std::span<const Value> values) {
    Value array = context.array();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto created = create_data_property(context, array, context.property_key(std::to_string(i)), values[i]);
        if (!created) return created.error();
        if (!*created) return EngineFailure{EngineFailureCode::InternalInvariant, "CreateArrayFromList failed to define element"};
    }
    return Completion::normal(array);
}

ExecutionResult create_list_from_array_like(Context& context, Value value) {
    const ExecutionResult object_result = to_object(context, value);
    if (!object_result || !object_result.completion().is_normal()) return object_result;
    const Value object = object_result.completion().value();
    const ExecutionResult length_result = get(context, object, context.property_key("length"));
    if (!length_result || !length_result.completion().is_normal()) return length_result;
    const ExecutionResult length_value = to_length(context, length_result.completion().value());
    if (!length_value || !length_value.completion().is_normal()) return length_value;
    const double length_number = length_value.completion().value().as_number();
    if (length_number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
        return type_error(context, "array-like length exceeds current list implementation limit");
    const auto length = static_cast<std::uint32_t>(length_number);
    Value list = context.array();
    for (std::uint32_t i = 0; i < length; ++i) {
        const ExecutionResult element = get(context, object, context.property_key(std::to_string(i)));
        if (!element || !element.completion().is_normal()) return element;
        const auto created = create_data_property(context, list, context.property_key(std::to_string(i)), element.completion().value());
        if (!created) return created.error();
        if (!*created) return EngineFailure{EngineFailureCode::InternalInvariant, "CreateListFromArrayLike failed to append"};
    }
    return Completion::normal(list);
}

} // namespace js::abstract_operations
