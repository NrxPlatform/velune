#pragma once

#include <string>
#include <js/environment.hpp>
#include <js/value.hpp>

namespace js {

class Context;
class Runtime;

class Realm final {
public:
    Realm(const Realm&) = delete;
    Realm& operator=(const Realm&) = delete;
    Realm(Realm&&) = delete;
    Realm& operator=(Realm&&) = delete;

    [[nodiscard]] Runtime& runtime() const noexcept { return *runtime_; }
    [[nodiscard]] Value global_object() const noexcept { return global_object_; }
    [[nodiscard]] Value object_prototype() const noexcept { return object_prototype_; }
    [[nodiscard]] GlobalEnvironmentRecord& global_environment() noexcept { return global_environment_; }
    [[nodiscard]] const GlobalEnvironmentRecord& global_environment() const noexcept { return global_environment_; }

private:
    friend class Context;
    friend class Runtime;

    explicit Realm(Runtime& runtime) noexcept : runtime_(&runtime) {}

    Runtime* runtime_;
    GlobalEnvironmentRecord global_environment_;
    Value global_object_{Value::undefined()};
    Value object_prototype_{Value::undefined()};
    Value function_prototype_{Value::undefined()};
    Value error_prototype_{Value::undefined()};
    Value type_error_prototype_{Value::undefined()};
    Value reference_error_prototype_{Value::undefined()};
    Value array_prototype_{Value::undefined()};
    Value promise_prototype_{Value::undefined()};
    Value regexp_prototype_{Value::undefined()};
    bool builtins_initialized_{false};
};

} // namespace js
