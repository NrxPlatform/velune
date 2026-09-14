#pragma once

#include <optional>

#include <js/value.hpp>

namespace js {

struct PropertyDescriptor final {
    std::optional<Value> value;
    std::optional<bool> writable;
    std::optional<Value> get;
    std::optional<Value> set;
    std::optional<bool> enumerable;
    std::optional<bool> configurable;

    [[nodiscard]] bool is_data_descriptor() const noexcept { return value.has_value() || writable.has_value(); }
    [[nodiscard]] bool is_accessor_descriptor() const noexcept { return get.has_value() || set.has_value(); }
    [[nodiscard]] bool is_generic_descriptor() const noexcept { return !is_data_descriptor() && !is_accessor_descriptor(); }
    [[nodiscard]] bool empty() const noexcept {
        return !value && !writable && !get && !set && !enumerable && !configurable;
    }

    [[nodiscard]] static PropertyDescriptor data(Value value,
                                                 bool writable = true,
                                                 bool enumerable = true,
                                                 bool configurable = true) {
        PropertyDescriptor descriptor;
        descriptor.value = value;
        descriptor.writable = writable;
        descriptor.enumerable = enumerable;
        descriptor.configurable = configurable;
        return descriptor;
    }

    [[nodiscard]] static PropertyDescriptor accessor(Value getter = Value::undefined(),
                                                     Value setter = Value::undefined(),
                                                     bool enumerable = true,
                                                     bool configurable = true) {
        PropertyDescriptor descriptor;
        descriptor.get = getter;
        descriptor.set = setter;
        descriptor.enumerable = enumerable;
        descriptor.configurable = configurable;
        return descriptor;
    }
};

} // namespace js
