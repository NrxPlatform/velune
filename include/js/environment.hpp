#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <js/completion.hpp>
#include <js/result.hpp>
#include <js/value.hpp>

namespace js {

class Context;

enum class BindingState : std::uint8_t {
    Uninitialized,
    InitializedMutable,
    InitializedImmutable,
};

struct BindingSlot final {
    Value value{Value::undefined()};
    BindingState state{BindingState::Uninitialized};
    bool immutable{false};

    [[nodiscard]] bool initialized() const noexcept { return state != BindingState::Uninitialized; }
    [[nodiscard]] bool mutable_binding() const noexcept { return state == BindingState::InitializedMutable; }
};

enum class EnvironmentKind : std::uint8_t {
    Declarative,
    Function,
    Global,
    Module,
    Object,
    Private,
};

// Semantic environment-record foundation. Optimized compiler slots remain valid lowerings;
// this record is used where named runtime bindings are required (Realm/global today, other
// environment kinds in later phases).
class EnvironmentRecord {
public:
    explicit EnvironmentRecord(EnvironmentKind kind, EnvironmentRecord* outer = nullptr) noexcept
        : kind_(kind), outer_(outer) {}
    virtual ~EnvironmentRecord() = default;

    [[nodiscard]] EnvironmentKind kind() const noexcept { return kind_; }
    [[nodiscard]] EnvironmentRecord* outer() const noexcept { return outer_; }
    [[nodiscard]] bool has_binding(std::string_view name) const noexcept;
    [[nodiscard]] Result<void> create_mutable_binding(std::string name, bool initialized = false);
    [[nodiscard]] Result<void> create_immutable_binding(std::string name, bool initialized = false);
    [[nodiscard]] Result<void> initialize_binding(std::string_view name, Value value);
    [[nodiscard]] Result<void> set_mutable_binding(std::string_view name, Value value);
    [[nodiscard]] Result<Value> get_binding_value(std::string_view name) const;
    [[nodiscard]] Result<bool> delete_binding(std::string_view name);
    [[nodiscard]] std::vector<Value> binding_values() const;

protected:
    struct NamedBinding final {
        BindingSlot slot;
        bool immutable{false};
        bool deletable{false};
    };
    std::unordered_map<std::string, NamedBinding> bindings_;

private:
    EnvironmentKind kind_;
    EnvironmentRecord* outer_;
};

class DeclarativeEnvironmentRecord final : public EnvironmentRecord {
public:
    explicit DeclarativeEnvironmentRecord(EnvironmentRecord* outer = nullptr) noexcept
        : EnvironmentRecord(EnvironmentKind::Declarative, outer) {}
};

class FunctionEnvironmentRecord final : public EnvironmentRecord {
public:
    explicit FunctionEnvironmentRecord(EnvironmentRecord* outer = nullptr) noexcept
        : EnvironmentRecord(EnvironmentKind::Function, outer) {}
};

class ModuleEnvironmentRecord final : public EnvironmentRecord {
public:
    explicit ModuleEnvironmentRecord(EnvironmentRecord* outer = nullptr) noexcept
        : EnvironmentRecord(EnvironmentKind::Module, outer) {}
};

class ObjectEnvironmentRecord final : public EnvironmentRecord {
public:
    explicit ObjectEnvironmentRecord(EnvironmentRecord* outer = nullptr) noexcept
        : EnvironmentRecord(EnvironmentKind::Object, outer) {}

    void attach_binding_object(Context& context, Value binding_object) noexcept;
    [[nodiscard]] bool has_object_binding(std::string_view name) const;
    [[nodiscard]] ExecutionResult get_object_binding_value(std::string_view name);
    [[nodiscard]] ExecutionResult set_object_binding_value(std::string_view name, Value value, bool strict);
    [[nodiscard]] Result<void> create_object_binding(std::string_view name, Value value, bool writable, bool enumerable, bool configurable);
    [[nodiscard]] std::vector<Value> object_binding_values() const;

private:
    Context* context_{nullptr};
    Value binding_object_{Value::undefined()};
};

class PrivateEnvironmentRecord final : public EnvironmentRecord {
public:
    explicit PrivateEnvironmentRecord(EnvironmentRecord* outer = nullptr) noexcept
        : EnvironmentRecord(EnvironmentKind::Private, outer) {}
};

class GlobalEnvironmentRecord final : public EnvironmentRecord {
public:
    GlobalEnvironmentRecord() noexcept
        : EnvironmentRecord(EnvironmentKind::Global), lexical_record_(this), object_record_(this) {}

    void attach_global_object(Context& context, Value global_object) noexcept;
    [[nodiscard]] bool has_binding(std::string_view name) const;
    [[nodiscard]] Result<void> create_global_var_binding(std::string name, Value value, bool configurable = false);
    [[nodiscard]] Result<void> create_global_constant_binding(std::string name, Value value);
    [[nodiscard]] Result<void> create_global_lexical_binding(std::string name, bool immutable);
    [[nodiscard]] Result<void> initialize_lexical_binding(std::string_view name, Value value);
    [[nodiscard]] ExecutionResult set_mutable_binding(std::string_view name, Value value, bool strict = false);
    [[nodiscard]] ExecutionResult get_binding_value(std::string_view name);
    [[nodiscard]] std::vector<Value> binding_values() const;

private:
    Context* context_{nullptr};
    DeclarativeEnvironmentRecord lexical_record_;
    ObjectEnvironmentRecord object_record_;
};

} // namespace js
