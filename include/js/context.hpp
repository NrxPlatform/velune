#pragma once

#include <string_view>
#include <optional>
#include <vector>

#include <js/native_function.hpp>
#include <js/property_key.hpp>
#include <js/property_descriptor.hpp>
#include <js/object.hpp>
#include <js/completion.hpp>
#include <js/result.hpp>
#include <js/runtime.hpp>
#include <js/realm.hpp>
#include <js/value.hpp>

namespace js {

class Context final {
public:
    explicit Context(Runtime& runtime) noexcept;
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    [[nodiscard]] Runtime& runtime() const noexcept { return *runtime_; }
    [[nodiscard]] Realm& realm() const noexcept { return *realm_; }
    [[nodiscard]] Value global_object() const noexcept { return realm_->global_object(); }
    [[nodiscard]] Value object_prototype();
    [[nodiscard]] Value error(std::string_view message = {});
    [[nodiscard]] Value type_error(std::string_view message = {});
    [[nodiscard]] Value reference_error(std::string_view message = {});

    [[nodiscard]] Value undefined() const noexcept { return Value::undefined(); }
    [[nodiscard]] Value null() const noexcept { return Value::null(); }
    [[nodiscard]] Value boolean(bool value) const noexcept { return Value::boolean(value); }
    [[nodiscard]] Value number(double value) const noexcept { return Value::number(value); }
    [[nodiscard]] Value string(std::string_view value) { return runtime_->make_string(value); }
    [[nodiscard]] Value symbol(std::optional<std::string> description = std::nullopt) { return runtime_->make_symbol(std::move(description)); }
    [[nodiscard]] Value symbol_for(std::string_view key) { return runtime_->symbol_for(key); }
    [[nodiscard]] std::optional<std::string_view> symbol_key_for(Value symbol) const noexcept { return runtime_->symbol_key_for(symbol); }
    [[nodiscard]] Value well_known_symbol(std::string_view name) { return runtime_->well_known_symbol(name); }
    [[nodiscard]] PropertyKey property_key(std::string_view key) const { return PropertyKey::atom(runtime_->intern_atom(key)); }
    [[nodiscard]] std::string_view property_key_text(PropertyKey key) const noexcept { return key.is_atom() ? runtime_->atom_text(key.atom_id()) : std::string_view{}; }
    [[nodiscard]] Result<PropertyKey> property_key(Value key) const;
    [[nodiscard]] Value object();
    [[nodiscard]] Value box_primitive(Value primitive);
    [[nodiscard]] Value array();
    [[nodiscard]] bool is_array(const Value& value) const noexcept;
    [[nodiscard]] Result<Value> object(Value prototype);
    [[nodiscard]] Value native_function(std::string_view name, std::uint32_t arity, NativeFunction function,
                                        ConstructorKind constructor_kind = ConstructorKind::None);
    [[nodiscard]] Result<Value> bind_function(Value target, Value bound_this, std::span<const Value> bound_arguments);
    [[nodiscard]] ExecutionResult invoke(Value callee, std::span<const Value> arguments, Value this_value = Value::undefined());
    [[nodiscard]] ExecutionResult construct(Value constructor, std::span<const Value> arguments = {});
    [[nodiscard]] ExecutionResult resume_generator(Value generator, Value input = Value::undefined());

    [[nodiscard]] ObjectKind object_kind(const Value& object) const;
    [[nodiscard]] Result<std::optional<PropertyDescriptor>> get_own_property_descriptor(const Value& object, PropertyKey key) const;
    [[nodiscard]] Result<std::optional<PropertyDescriptor>> get_own_property_descriptor(const Value& object, std::string_view key) const { return get_own_property_descriptor(object, property_key(key)); }
    [[nodiscard]] Result<bool> define_own_property(const Value& object, PropertyKey key, const PropertyDescriptor& descriptor) const;
    [[nodiscard]] Result<bool> define_own_property(const Value& object, std::string_view key, const PropertyDescriptor& descriptor) const { return define_own_property(object, property_key(key), descriptor); }
    [[nodiscard]] Result<bool> has_property(const Value& object, PropertyKey key) const;
    [[nodiscard]] Result<bool> has_property(const Value& object, std::string_view key) const { return has_property(object, property_key(key)); }
    [[nodiscard]] Result<bool> delete_property(const Value& object, PropertyKey key) const;
    [[nodiscard]] ExecutionResult delete_property_semantic(const Value& object, PropertyKey key);
    [[nodiscard]] Result<bool> delete_property(const Value& object, std::string_view key) const { return delete_property(object, property_key(key)); }
    [[nodiscard]] Result<std::vector<PropertyKey>> own_property_keys(const Value& object) const;
    [[nodiscard]] Result<bool> is_extensible(const Value& object) const;
    [[nodiscard]] Result<bool> prevent_extensions(const Value& object) const;

    [[nodiscard]] Result<Value> get_own_property(const Value& object, PropertyKey key) const;
    [[nodiscard]] Result<Value> get_own_property(const Value& object, std::string_view key) const { return get_own_property(object, property_key(key)); }
    [[nodiscard]] ExecutionResult get_property_semantic(const Value& object, PropertyKey key, Value receiver);
    [[nodiscard]] ExecutionResult get_property_semantic(const Value& object, PropertyKey key) { return get_property_semantic(object, key, object); }
    [[nodiscard]] Result<Value> get_property(const Value& object, PropertyKey key);
    [[nodiscard]] Result<Value> get_property(const Value& object, std::string_view key) { return get_property(object, property_key(key)); }
    [[nodiscard]] Result<void> set_own_property(const Value& object, PropertyKey key, Value value) const;
    [[nodiscard]] Result<void> set_own_property(const Value& object, std::string_view key, Value value) const { return set_own_property(object, property_key(key), value); }
    [[nodiscard]] ExecutionResult set_property_semantic(const Value& object, PropertyKey key, Value value, Value receiver);
    [[nodiscard]] ExecutionResult set_property_semantic(const Value& object, PropertyKey key, Value value) { return set_property_semantic(object, key, value, object); }
    [[nodiscard]] Result<void> set_property(const Value& object, PropertyKey key, Value value);
    [[nodiscard]] Result<void> set_property(const Value& object, std::string_view key, Value value) { return set_property(object, property_key(key), value); }
    [[nodiscard]] Result<Value> function_bound_target(const Value& function) const;
    [[nodiscard]] Result<Value> get_prototype(const Value& object) const;
    [[nodiscard]] Result<void> set_prototype(const Value& object, Value prototype) const;
    [[nodiscard]] ExecutionResult get_element_semantic(const Value& object, Value key);
    [[nodiscard]] Result<Value> get_element(const Value& object, Value key);
    [[nodiscard]] ExecutionResult set_element_semantic(const Value& object, Value key, Value value);
    [[nodiscard]] Result<void> set_element(const Value& object, Value key, Value value);
    [[nodiscard]] Result<Value> get_global(std::string_view name);
    [[nodiscard]] Result<Value> array_push(Value array, Value value) const;
    [[nodiscard]] Result<Value> array_pop(Value array) const;
    [[nodiscard]] Result<Value> array_iterator(Value array);
    [[nodiscard]] Result<Value> array_iterator_next(Value iterator);
    [[nodiscard]] bool is_promise(const Value& value) const noexcept;
    [[nodiscard]] Result<Value> promise_resolve(Value value);
    [[nodiscard]] Result<Value> promise_reject(Value value);
    [[nodiscard]] Result<Value> promise_then(Value promise, Value on_fulfilled);
    [[nodiscard]] Result<Value> promise_catch(Value promise, Value on_rejected);
    [[nodiscard]] Result<Value> promise_result(Value promise) const;
    [[nodiscard]] Result<Value> regexp(std::string_view pattern, std::string_view flags);
    [[nodiscard]] bool is_regexp(const Value& value) const noexcept;
    [[nodiscard]] Result<Value> regexp_test(Value regexp, Value input);
    [[nodiscard]] Result<Value> regexp_exec(Value regexp, Value input);

    // Heap values may cross Context boundaries only when both Contexts share a Runtime.
    [[nodiscard]] Result<void> validate(const Value& value) const;

private:
    friend class VM;
    void ensure_builtins();
    void ensure_builtins(Realm& realm);
    [[nodiscard]] Realm& active_realm() const noexcept { return execution_realm_ == nullptr ? *realm_ : *execution_realm_; }
    [[nodiscard]] Value object_in_realm(Realm& realm);
    [[nodiscard]] Result<Value> object_in_realm(Realm& realm, Value prototype);
    [[nodiscard]] Value array_in_realm(Realm& realm);
    [[nodiscard]] Value native_function_in_realm(Realm& realm, std::string_view name, std::uint32_t arity, NativeFunction function,
                                                 ConstructorKind constructor_kind = ConstructorKind::None);
    Runtime* runtime_;
    Realm* realm_;
    Realm* execution_realm_{nullptr};
};

} // namespace js
