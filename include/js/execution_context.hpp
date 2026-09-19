#pragma once

namespace js {

class Realm;
class EnvironmentRecord;
class PrivateEnvironmentRecord;

namespace detail {
struct HeapFunction;
struct HeapModuleEnvironment;
struct HeapDynamicEnvironment;
}

// Compact runtime representation of the currently executing ECMAScript context.
// Null lexical/variable pointers mean the environment was compiler-lowered to verified
// local/upvalue slots rather than materialized as a named heap record.
struct ExecutionContext final {
    Realm* realm{nullptr};
    const detail::HeapFunction* function{nullptr};
    detail::HeapModuleEnvironment* module_environment{nullptr};
    detail::HeapDynamicEnvironment* dynamic_environment{nullptr};
    EnvironmentRecord* lexical_environment{nullptr};
    EnvironmentRecord* variable_environment{nullptr};
    PrivateEnvironmentRecord* private_environment{nullptr};
};

} // namespace js
