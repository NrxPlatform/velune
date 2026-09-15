#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <js/atom_table.hpp>
#include <js/environment.hpp>
#include <js/function_semantics.hpp>
#include <js/native_function.hpp>
#include <js/result.hpp>
#include <js/realm.hpp>
#include <js/value.hpp>

namespace js {

class Context;
class VM;
class Realm;

namespace bytecode {
class BytecodeChunk;
}

namespace compiler {
class Compiler;
}
namespace module {
class ModuleSystem;
}

namespace detail {
struct HeapCell;
struct HeapFunction;
struct HeapUpvalue;
struct HeapObject;
struct HeapModuleEnvironment;
struct SymbolRecord;
}

class Runtime;

class PersistentRoot final {
public:
    PersistentRoot() noexcept = default;
    ~PersistentRoot();

    PersistentRoot(const PersistentRoot&) = delete;
    PersistentRoot& operator=(const PersistentRoot&) = delete;

    PersistentRoot(PersistentRoot&& other) noexcept;
    PersistentRoot& operator=(PersistentRoot&& other) noexcept;

    [[nodiscard]] Value value() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return runtime_ != nullptr; }
    [[nodiscard]] Result<void> set(Value value);
    void reset() noexcept;

private:
    friend class Runtime;
    PersistentRoot(Runtime& runtime, std::uint64_t id) noexcept : runtime_(&runtime), id_(id) {}

    Runtime* runtime_{nullptr};
    std::uint64_t id_{0};
};


class BytecodeRoot final {
public:
    BytecodeRoot() noexcept = default;
    ~BytecodeRoot();

    BytecodeRoot(const BytecodeRoot&) = delete;
    BytecodeRoot& operator=(const BytecodeRoot&) = delete;
    BytecodeRoot(BytecodeRoot&& other) noexcept;
    BytecodeRoot& operator=(BytecodeRoot&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return runtime_ != nullptr; }
    void reset() noexcept;

private:
    friend class Runtime;
    BytecodeRoot(Runtime& runtime, std::uint64_t id) noexcept : runtime_(&runtime), id_(id) {}
    Runtime* runtime_{nullptr};
    std::uint64_t id_{0};
};

struct GarbageCollectionStats final {
    std::size_t before{0};
    std::size_t marked{0};
    std::size_t collected{0};
    std::size_t after{0};
};

class Runtime final {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] std::size_t heap_cell_count() const noexcept;
    [[nodiscard]] Result<PersistentRoot> root(Value value);
    [[nodiscard]] Result<BytecodeRoot> root_bytecode(const bytecode::BytecodeChunk& chunk);
    [[nodiscard]] GarbageCollectionStats collect_garbage();
    [[nodiscard]] std::size_t collection_count() const noexcept { return collection_count_; }
    [[nodiscard]] std::size_t pending_job_count() const noexcept { return jobs_.size(); }
    [[nodiscard]] Result<std::size_t> run_jobs(Context& context);
    [[nodiscard]] std::size_t realm_count() const noexcept { return realms_.size(); }
    [[nodiscard]] std::size_t atom_count() const noexcept { return atoms_.size(); }

private:
    friend class Context;
    friend class Value;
    friend class PersistentRoot;
    friend class BytecodeRoot;
    friend class compiler::Compiler;
    friend class module::ModuleSystem;
    friend class VM;

    [[nodiscard]] Value make_string(std::string_view text);
    [[nodiscard]] AtomId intern_atom(std::string_view text) { return atoms_.intern(text); }
    [[nodiscard]] std::string_view atom_text(AtomId id) const noexcept { return atoms_.text(id); }
    [[nodiscard]] Value make_symbol(std::optional<std::string> description = std::nullopt);
    [[nodiscard]] Value symbol_for(std::string_view key);
    [[nodiscard]] std::optional<std::string_view> symbol_key_for(Value symbol) const noexcept;
    [[nodiscard]] Value well_known_symbol(std::string_view name);
    [[nodiscard]] Realm& create_realm();
    [[nodiscard]] Value make_object(Realm& realm);
    [[nodiscard]] Value make_array(Realm& realm);
    [[nodiscard]] Value make_promise(Realm& realm);
    [[nodiscard]] Value make_function(Realm& realm, std::string name, std::uint32_t function_length, bytecode::BytecodeChunk chunk,
                                      ConstructorKind constructor_kind = ConstructorKind::Base, bool generator = false,
                                      std::optional<std::uint32_t> arguments_slot = std::nullopt,
                                      ThisMode this_mode = ThisMode::Global, bool method = false,
                                      std::optional<std::uint32_t> parameter_count = std::nullopt,
                                      bool simple_parameter_list = true, bool strict = false,
                                      std::vector<std::string> parameter_names = {});
    [[nodiscard]] Value make_native_function(Realm& realm, std::string name, std::uint32_t arity, NativeFunction function,
                                             ConstructorKind constructor_kind = ConstructorKind::None, NativeFunction construct_function = nullptr);
    [[nodiscard]] Value make_bound_function(Realm& realm, Value target, Value bound_this, std::span<const Value> bound_arguments);
    [[nodiscard]] Value make_closure(const detail::HeapFunction& prototype, std::vector<detail::HeapUpvalue*> upvalues, detail::HeapModuleEnvironment* module_environment = nullptr);
    [[nodiscard]] detail::HeapModuleEnvironment* make_module_environment(std::size_t binding_count);
    [[nodiscard]] detail::HeapUpvalue* make_upvalue(BindingSlot* location);
    [[nodiscard]] bool owns(const Value& value) const noexcept;

    void register_context() noexcept;
    void unregister_context() noexcept;
    void register_vm(VM& vm);
    void unregister_vm(VM& vm) noexcept;
    void register_module_environment(detail::HeapModuleEnvironment& environment);
    void unregister_module_environment(detail::HeapModuleEnvironment& environment) noexcept;

    [[nodiscard]] Value persistent_root_value(std::uint64_t id) const noexcept;
    [[nodiscard]] Result<void> set_persistent_root(std::uint64_t id, Value value);
    void release_persistent_root(std::uint64_t id) noexcept;
    void release_bytecode_root(std::uint64_t id) noexcept;

    void mark_roots();
    void mark_value(Value value);
    void mark_cell(detail::HeapCell* cell);
    void trace_cell(detail::HeapCell* cell);
    void mark_chunk(const bytecode::BytecodeChunk& chunk);
    void mark_vm(const VM& vm);
    [[nodiscard]] Result<void> settle_promise(Value promise, bool fulfilled, Value result);
    void enqueue_promise_reaction(bool fulfilled, Value result, Value on_fulfilled, Value on_rejected, Value next_promise);

    AtomTable atoms_;
    std::vector<std::unique_ptr<detail::SymbolRecord>> symbols_;
    std::unordered_map<std::string, SymbolId> symbol_registry_;
    std::unordered_map<std::string, SymbolId> well_known_symbols_;
    std::vector<std::unique_ptr<detail::HeapCell>> heap_;
    std::vector<std::unique_ptr<Realm>> realms_;
    std::unordered_map<std::uint64_t, Value> persistent_roots_;
    std::unordered_map<std::uint64_t, const bytecode::BytecodeChunk*> bytecode_roots_;
    std::vector<VM*> active_vms_;
    std::vector<detail::HeapModuleEnvironment*> module_environments_;
    struct PromiseReactionJob final {
        bool fulfilled{false};
        Value result{Value::undefined()};
        Value on_fulfilled{Value::undefined()};
        Value on_rejected{Value::undefined()};
        Value next_promise{Value::undefined()};
    };

    std::vector<detail::HeapCell*> mark_stack_;
    std::deque<PromiseReactionJob> jobs_;
    std::size_t active_contexts_{0};
    std::uint64_t next_root_id_{1};
    std::size_t collection_count_{0};
};

} // namespace js
