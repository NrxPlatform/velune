#pragma once

#include <cstdint>

namespace js {

enum class ObjectKind : std::uint8_t {
    Ordinary,
    Array,
    Function,
    Promise,
    Generator,
    RegExp,
    StringExotic,
    Arguments,
    MappedArguments,
    ModuleNamespace,
    Proxy,
    TypedArray,
};

} // namespace js
