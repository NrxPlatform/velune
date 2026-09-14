#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <js/property_key.hpp>

namespace js {

class Runtime;
class VM;

namespace detail {
struct HeapString;
struct HeapFunction;
struct HeapObject;
struct SymbolRecord;
struct ValueAccess;
}

enum class ValueTag : std::uint8_t {
    undefined,
    null,
    boolean,
    number,
    string,
    symbol,
    function,
    object,
};

class Value {
public:
    Value() noexcept = default;

    [[nodiscard]] static Value undefined() noexcept;
    [[nodiscard]] static Value null() noexcept;
    [[nodiscard]] static Value boolean(bool value) noexcept;
    [[nodiscard]] static Value number(double value) noexcept;

    [[nodiscard]] ValueTag tag() const noexcept { return tag_; }
    [[nodiscard]] bool is_undefined() const noexcept { return tag_ == ValueTag::undefined; }
    [[nodiscard]] bool is_null() const noexcept { return tag_ == ValueTag::null; }
    [[nodiscard]] bool is_boolean() const noexcept { return tag_ == ValueTag::boolean; }
    [[nodiscard]] bool is_number() const noexcept { return tag_ == ValueTag::number; }
    [[nodiscard]] bool is_string() const noexcept { return tag_ == ValueTag::string; }
    [[nodiscard]] bool is_symbol() const noexcept { return tag_ == ValueTag::symbol; }
    [[nodiscard]] bool is_function() const noexcept { return tag_ == ValueTag::function; }
    [[nodiscard]] bool is_object() const noexcept { return tag_ == ValueTag::object; }
    [[nodiscard]] bool is_object_like() const noexcept { return is_object() || is_function(); }
    [[nodiscard]] bool is_constructor() const noexcept;
    [[nodiscard]] bool is_heap_backed() const noexcept { return is_string() || is_function() || is_object(); }

    [[nodiscard]] bool as_boolean() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] std::string_view as_string() const;
    [[nodiscard]] SymbolId as_symbol_id() const;
    [[nodiscard]] std::string_view symbol_description() const;
    [[nodiscard]] bool same_heap_identity(const Value& other) const noexcept;

    // Diagnostic representation only. This is deliberately NOT JavaScript ToString().
    [[nodiscard]] std::string to_debug_string() const;

private:
    friend class Runtime;
    friend class Context;
    friend class VM;
    friend struct detail::ValueAccess;

    explicit Value(const detail::HeapString* string) noexcept;
    explicit Value(const detail::SymbolRecord* symbol) noexcept;
    explicit Value(const detail::HeapFunction* function) noexcept;
    explicit Value(detail::HeapObject* object) noexcept;
    [[nodiscard]] const Runtime* owning_runtime() const noexcept;
    [[nodiscard]] const detail::HeapFunction* as_heap_function() const;
    [[nodiscard]] detail::HeapObject* as_heap_object() const;

    ValueTag tag_{ValueTag::undefined};
    union Payload {
        bool boolean;
        double number;
        const detail::HeapString* string;
        const detail::SymbolRecord* symbol;
        const detail::HeapFunction* function;
        detail::HeapObject* object;

        constexpr Payload() : number(0.0) {}
    } payload_{};
};

[[nodiscard]] std::string_view value_tag_name(ValueTag tag) noexcept;

} // namespace js
