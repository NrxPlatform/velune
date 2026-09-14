#pragma once

#include <span>

#include <js/completion.hpp>
#include <js/value.hpp>

namespace js {

class Context;

using NativeFunction = ExecutionResult (*)(Context& context, Value this_value, std::span<const Value> arguments);

} // namespace js
