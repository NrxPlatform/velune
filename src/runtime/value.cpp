#include <js/value.hpp>

#include <cassert>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "detail/heap.hpp"

namespace js {

Value Value::undefined() noexcept { return {}; }

Value Value::null() noexcept {
    Value value;
    value.tag_ = ValueTag::null;
    return value;
}

Value Value::boolean(bool boolean_value) noexcept {
    Value value;
    value.tag_ = ValueTag::boolean;
    value.payload_.boolean = boolean_value;
    return value;
}

Value Value::number(double number_value) noexcept {
    Value value;
    value.tag_ = ValueTag::number;
    value.payload_.number = number_value;
    return value;
}

Value::Value(const detail::HeapString* string) noexcept {
    assert(string != nullptr);
    tag_ = ValueTag::string;
    payload_.string = string;
}

Value::Value(const detail::SymbolRecord* symbol) noexcept {
    assert(symbol != nullptr);
    tag_ = ValueTag::symbol;
    payload_.symbol = symbol;
}

Value::Value(detail::HeapObject* object) noexcept {
    assert(object != nullptr);
    tag_ = ValueTag::object;
    payload_.object = object;
}

Value::Value(const detail::HeapFunction* function) noexcept {
    assert(function != nullptr);
    tag_ = ValueTag::function;
    payload_.function = function;
}

bool Value::as_boolean() const {
    if (!is_boolean()) throw std::logic_error("Value::as_boolean() called for non-boolean value");
    return payload_.boolean;
}

double Value::as_number() const {
    if (!is_number()) throw std::logic_error("Value::as_number() called for non-number value");
    return payload_.number;
}

std::string_view Value::as_string() const {
    if (!is_string()) throw std::logic_error("Value::as_string() called for non-string value");
    return payload_.string->text;
}

SymbolId Value::as_symbol_id() const {
    if (!is_symbol()) throw std::logic_error("Value::as_symbol_id() called for non-symbol value");
    return payload_.symbol->id;
}

std::string_view Value::symbol_description() const {
    if (!is_symbol()) throw std::logic_error("Value::symbol_description() called for non-symbol value");
    return payload_.symbol->description ? std::string_view(*payload_.symbol->description) : std::string_view{};
}

bool Value::same_heap_identity(const Value& other) const noexcept {
    if (tag_ != other.tag_) return false;
    switch (tag_) {
    case ValueTag::string: return payload_.string == other.payload_.string;
    case ValueTag::symbol: return payload_.symbol == other.payload_.symbol;
    case ValueTag::function: return payload_.function == other.payload_.function;
    case ValueTag::object: return payload_.object == other.payload_.object;
    default: return false;
    }
}

const detail::HeapFunction* Value::as_heap_function() const {
    if (!is_function()) throw std::logic_error("Value::as_heap_function() called for non-function value");
    return payload_.function;
}

detail::HeapObject* Value::as_heap_object() const {
    if (!is_object()) throw std::logic_error("Value::as_heap_object() called for non-object value");
    return payload_.object;
}

const Runtime* Value::owning_runtime() const noexcept {
    switch (tag_) {
    case ValueTag::string: return payload_.string->owner;
    case ValueTag::symbol: return payload_.symbol->owner;
    case ValueTag::function: return payload_.function->owner;
    case ValueTag::object: return payload_.object->owner;
    default: return nullptr;
    }
}

bool Value::is_constructor() const noexcept {
    if (!is_function()) return false;
    const auto* function = as_heap_function();
    if (!function->bound_target.is_undefined()) return function->bound_target.is_constructor();
    return function->code->constructor_kind != ConstructorKind::None;
}

std::string Value::to_debug_string() const {
    switch (tag_) {
    case ValueTag::undefined: return "undefined";
    case ValueTag::null: return "null";
    case ValueTag::boolean: return payload_.boolean ? "true" : "false";
    case ValueTag::number: {
        if (std::isnan(payload_.number)) return "NaN";
        if (std::isinf(payload_.number)) return payload_.number < 0 ? "-Infinity" : "Infinity";
        if (payload_.number == 0.0 && std::signbit(payload_.number)) return "-0";
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<double>::max_digits10) << payload_.number;
        return out.str();
    }
    case ValueTag::string: return std::string{"\""} + payload_.string->text + "\"";
    case ValueTag::symbol: return "Symbol(" + std::string(symbol_description()) + ")";
    case ValueTag::function: return "<function " + payload_.function->code->name + "/" + std::to_string(payload_.function->code->arity) + ">";
    case ValueTag::object: return "<object properties=" + std::to_string(payload_.object->properties.size()) + ">";
    }
    return "<invalid-value>";
}

std::string_view value_tag_name(ValueTag tag) noexcept {
    switch (tag) {
    case ValueTag::undefined: return "undefined";
    case ValueTag::null: return "null";
    case ValueTag::boolean: return "boolean";
    case ValueTag::number: return "number";
    case ValueTag::string: return "string";
    case ValueTag::symbol: return "symbol";
    case ValueTag::function: return "function";
    case ValueTag::object: return "object";
    }
    return "unknown";
}

} // namespace js
