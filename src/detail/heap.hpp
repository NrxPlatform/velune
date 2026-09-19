#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include <js/bytecode/chunk.hpp>
#include <js/environment.hpp>
#include <js/dynamic_reference.hpp>
#include <js/function_semantics.hpp>
#include <js/native_function.hpp>
#include <js/property_key.hpp>
#include <js/property_descriptor.hpp>
#include <js/object.hpp>
#include <js/value.hpp>

namespace js {

class Runtime;
class Realm;

namespace detail {

enum class HeapKind {
    string,
    function,
    upvalue,
    object,
    module_environment,
    dynamic_environment,
};


struct SymbolRecord final {
    Runtime* owner{nullptr};
    SymbolId id{0};
    std::optional<std::string> description;
    std::optional<std::string> registry_key;
    std::optional<std::string> well_known_name;
};

struct HeapCell {
    HeapCell(Runtime* owner_runtime, HeapKind heap_kind) noexcept
        : owner(owner_runtime), kind(heap_kind) {}
    virtual ~HeapCell() = default;

    Runtime* owner;
    HeapKind kind;
    bool marked{false};
};

struct HeapString final : HeapCell {
    HeapString(Runtime* owner_runtime, std::string string_text)
        : HeapCell(owner_runtime, HeapKind::string), text(std::move(string_text)) {}

    std::string text;
};

struct FunctionCode final {
    FunctionCode(std::string function_name, std::uint32_t function_length, bytecode::BytecodeChunk function_chunk,
                 ConstructorKind constructor = ConstructorKind::Base, bool is_generator = false,
                 std::optional<std::uint32_t> function_arguments_slot = std::nullopt,
                 ThisMode this_binding_mode = ThisMode::Global, bool is_method = false,
                 std::optional<std::uint32_t> formal_parameter_count = std::nullopt,
                 bool simple_parameters = true, bool is_strict = false,
                 std::vector<std::string> formal_parameter_names = {})
        : name(std::move(function_name)), arity(function_length), parameter_count(formal_parameter_count.value_or(function_length)),
          parameter_names(std::move(formal_parameter_names)), chunk(std::move(function_chunk)), constructor_kind(constructor), generator(is_generator), arguments_slot(function_arguments_slot),
          this_mode(this_binding_mode), strict(is_strict), method(is_method), simple_parameter_list(simple_parameters) {}

    FunctionCode(std::string function_name, std::uint32_t function_arity, NativeFunction native_function,
                 ConstructorKind constructor = ConstructorKind::None, NativeFunction native_constructor = nullptr)
        : name(std::move(function_name)), arity(function_arity), native(native_function), native_construct(native_constructor), constructor_kind(constructor) {}

    FunctionCode(std::string function_name, std::uint32_t function_arity)
        : name(std::move(function_name)), arity(function_arity) {}

    [[nodiscard]] bool is_native() const noexcept { return native != nullptr; }

    std::string name;
    std::uint32_t arity{0};
    std::uint32_t parameter_count{0};
    std::vector<std::string> parameter_names;
    bytecode::BytecodeChunk chunk;
    NativeFunction native{nullptr};
    NativeFunction native_construct{nullptr};
    ConstructorKind constructor_kind{ConstructorKind::Base};
    bool generator{false};
    std::optional<std::uint32_t> arguments_slot;
    ThisMode this_mode{ThisMode::Global};
    bool strict{false};
    bool method{false};
    bool simple_parameter_list{true};
};

struct HeapUpvalue final : HeapCell {
    HeapUpvalue(Runtime* owner_runtime, BindingSlot* open_location)
        : HeapCell(owner_runtime, HeapKind::upvalue), location(open_location) {}

    [[nodiscard]] Value get() const noexcept { return location->value; }
    [[nodiscard]] BindingState state() const noexcept { return location->state; }
    [[nodiscard]] bool immutable() const noexcept { return location->immutable; }
    void set(Value value) noexcept { location->value = value; }
    void initialize(Value value) noexcept {
        location->value = value;
        location->state = location->immutable ? BindingState::InitializedImmutable : BindingState::InitializedMutable;
    }
    void close() noexcept {
        closed = *location;
        location = &closed;
    }

    BindingSlot* location;
    BindingSlot closed{};
};

enum class PromiseState : std::uint8_t { pending, fulfilled, rejected };

struct PromiseReaction final {
    Value on_fulfilled{Value::undefined()};
    Value on_rejected{Value::undefined()};
    Value next_promise{Value::undefined()};
};

enum class GeneratorStateKind : std::uint8_t { suspended_start, suspended_yield, executing, completed };

struct GeneratorState final {
    Realm* realm{nullptr};
    const struct HeapFunction* function{nullptr};
    GeneratorStateKind state{GeneratorStateKind::suspended_start};
    std::shared_ptr<const FunctionCode> code;
    std::size_t pc{0};
    std::size_t last_instruction_pc{0};
    Value this_value{Value::undefined()};
    std::vector<BindingSlot> locals;
    std::vector<Value> actual_arguments;
    std::vector<Value> stack;
    std::vector<HeapUpvalue*> upvalues;
    std::vector<HeapUpvalue*> captured_locals;
    std::vector<DynamicBindingReference> retained_references;
    std::vector<std::pair<std::size_t, std::size_t>> with_regions;
    struct HeapModuleEnvironment* module_environment{nullptr};
    struct HeapDynamicEnvironment* dynamic_environment{nullptr};
};

struct HeapObject final : HeapCell {
    explicit HeapObject(Runtime* owner_runtime, Realm* owner_realm, bool array_object = false, bool promise_object = false)
        : HeapCell(owner_runtime, HeapKind::object), realm(owner_realm), is_array(array_object), is_promise(promise_object) {}

    Realm* realm;

    ObjectKind object_kind{ObjectKind::Ordinary};
    bool extensible{true};
    bool is_array{false};
    bool is_promise{false};
    bool is_generator{false};
    bool is_regexp{false};
    std::string regexp_pattern;
    std::string regexp_flags;
    bool regexp_global{false};
    bool regexp_ignore_case{false};
    bool regexp_multiline{false};
    std::size_t regexp_last_index{0};
    GeneratorState generator_state{};
    PromiseState promise_state{PromiseState::pending};
    Value promise_result{Value::undefined()};
    std::optional<Value> boxed_primitive;
    std::vector<HeapUpvalue*> mapped_argument_upvalues;
    std::vector<PromiseReaction> promise_reactions;
    std::vector<std::optional<Value>> elements;
    std::uint32_t array_length{0};
    bool array_length_writable{true};
    std::unordered_map<PropertyKey, PropertyDescriptor, PropertyKeyHash> properties;
    std::vector<PropertyKey> property_order;
    Value prototype{Value::null()};
};


struct ModuleBindingCell final {
    HeapUpvalue* local_upvalue{nullptr};
    struct HeapModuleEnvironment* target_environment{nullptr};
    std::uint32_t target_index{0};
};

struct HeapModuleEnvironment final : HeapCell {
    HeapModuleEnvironment(Runtime* owner_runtime, std::size_t count)
        : HeapCell(owner_runtime, HeapKind::module_environment), bindings(count) {}

    [[nodiscard]] Result<Value> get(std::uint32_t index) const {
        const ModuleBindingCell* cell = &bindings[index];
        while (cell->target_environment != nullptr) {
            cell = &cell->target_environment->bindings[cell->target_index];
        }
        if (cell->local_upvalue == nullptr || cell->local_upvalue->state() == BindingState::Uninitialized)
            return Error{ErrorCode::reference_error, "cannot access module binding before initialization"};
        return cell->local_upvalue->get();
    }

    [[nodiscard]] Result<void> set(std::uint32_t index, Value value) {
        ModuleBindingCell* cell = &bindings[index];
        const bool imported = cell->target_environment != nullptr;
        while (cell->target_environment != nullptr) {
            cell = &cell->target_environment->bindings[cell->target_index];
        }
        if (imported) return Error{ErrorCode::type_error, "assignment to immutable import binding"};
        if (cell->local_upvalue == nullptr || cell->local_upvalue->state() == BindingState::Uninitialized)
            return Error{ErrorCode::reference_error, "cannot assign to module binding before initialization"};
        if (cell->local_upvalue->immutable()) return Error{ErrorCode::type_error, "assignment to immutable module binding"};
        cell->local_upvalue->set(value);
        return {};
    }

    std::vector<ModuleBindingCell> bindings;
};

// GC-owned lifetime anchor for a materialized dynamic lexical environment.
// Identifier resolution and WithStatement lowering are intentionally added in later patches.
struct HeapDynamicEnvironment final : HeapCell {
    HeapDynamicEnvironment(Runtime* runtime, Value object, HeapDynamicEnvironment* enclosing = nullptr)
        : HeapCell(runtime, HeapKind::dynamic_environment), binding_object(object), outer(enclosing) {}

    Value binding_object{Value::undefined()};
    HeapDynamicEnvironment* outer{nullptr};
};

struct HeapFunction final : HeapCell {
    HeapFunction(Runtime* owner_runtime, Realm* owner_realm, std::shared_ptr<const FunctionCode> function_code, std::vector<HeapUpvalue*> function_upvalues = {}, HeapModuleEnvironment* function_module_environment = nullptr)
        : HeapCell(owner_runtime, HeapKind::function), realm(owner_realm), code(std::move(function_code)), upvalues(std::move(function_upvalues)), module_environment(function_module_environment) {}

    Realm* realm;
    ObjectKind object_kind{ObjectKind::Function};
    bool extensible{true};
    std::shared_ptr<const FunctionCode> code;
    std::vector<HeapUpvalue*> upvalues;
    struct HeapModuleEnvironment* module_environment{nullptr};
    HeapDynamicEnvironment* captured_dynamic_environment{nullptr};
    std::unordered_map<PropertyKey, PropertyDescriptor, PropertyKeyHash> properties;
    std::vector<PropertyKey> property_order;
    Value prototype{Value::null()};
    std::optional<Value> lexical_this;
    Value home_object{Value::undefined()};
    Value bound_target{Value::undefined()};
    Value bound_this{Value::undefined()};
    std::vector<Value> bound_arguments;
};

} // namespace detail
} // namespace js
