#pragma once
#include <cstdint>
namespace js {
enum class ThisMode : std::uint8_t { Global, Strict, Lexical };
enum class ConstructorKind : std::uint8_t { None, Base, Derived };
}
