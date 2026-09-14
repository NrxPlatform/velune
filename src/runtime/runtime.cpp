#include <js/runtime.hpp>

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <js/bytecode/chunk.hpp>
#include <js/context.hpp>
#include <js/error.hpp>
#include <js/vm/vm.hpp>

#include "detail/heap.hpp"

namespace js {

PersistentRoot::~PersistentRoot() {
    reset();
}

PersistentRoot::PersistentRoot(PersistentRoot&& other) noexcept
    : runtime_(std::exchange(other.runtime_, nullptr)), id_(std::exchange(other.id_, 0)) {}

PersistentRoot& PersistentRoot::operator=(PersistentRoot&& other) noexcept {
    if (this == &other) return *this;
    reset();
    runtime_ = std::exchange(other.runtime_, nullptr);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

Value PersistentRoot::value() const noexcept {
    if (runtime_ == nullptr) return Value::undefined();
    return runtime_->persistent_root_value(id_);
}

Result<void> PersistentRoot::set(Value value) {
    if (runtime_ == nullptr) return Error{ErrorCode::internal, "cannot set a released persistent root"};
    return runtime_->set_persistent_root(id_, value);
}

void PersistentRoot::reset() noexcept {
    if (runtime_ == nullptr) return;
    runtime_->release_persistent_root(id_);
    runtime_ = nullptr;
    id_ = 0;
}

BytecodeRoot::~BytecodeRoot() {
    reset();
}

BytecodeRoot::BytecodeRoot(BytecodeRoot&& other) noexcept
    : runtime_(std::exchange(other.runtime_, nullptr)), id_(std::exchange(other.id_, 0)) {}

BytecodeRoot& BytecodeRoot::operator=(BytecodeRoot&& other) noexcept {
    if (this == &other) return *this;
    reset();
    runtime_ = std::exchange(other.runtime_, nullptr);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

void BytecodeRoot::reset() noexcept {
    if (runtime_ == nullptr) return;
    runtime_->release_bytecode_root(id_);
    runtime_ = nullptr;
    id_ = 0;
}

Runtime::Runtime() = default;

Runtime::~Runtime() {
    assert(active_contexts_ == 0 && "Runtime destroyed while Context objects are still alive");
    assert(active_vms_.empty() && "Runtime destroyed while VM objects are actively executing");
    assert(persistent_roots_.empty() && "Runtime destroyed while PersistentRoot objects are still alive");
    assert(bytecode_roots_.empty() && "Runtime destroyed while BytecodeRoot objects are still alive");
    assert(module_environments_.empty() && "Runtime destroyed while ModuleSystem objects are still alive");
}

std::size_t Runtime::heap_cell_count() const noexcept {
    return heap_.size();
}

Result<PersistentRoot> Runtime::root(Value value) {
    if (!owns(value)) return Error{ErrorCode::runtime_mismatch, "persistent root Value belongs to a different Runtime"};
    const std::uint64_t id = next_root_id_++;
    persistent_roots_.emplace(id, value);
    return PersistentRoot(*this, id);
}

Result<BytecodeRoot> Runtime::root_bytecode(const bytecode::BytecodeChunk& chunk) {
    for (const Value constant : chunk.constants()) {
        if (!owns(constant)) return Error{ErrorCode::runtime_mismatch, "bytecode root contains a Value from a different Runtime"};
    }
    const std::uint64_t id = next_root_id_++;
    bytecode_roots_.emplace(id, &chunk);
    return BytecodeRoot(*this, id);
}

Value Runtime::make_string(std::string_view text) {
    auto cell = std::make_unique<detail::HeapString>(this, std::string(text));
    const auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return Value(raw);
}


Value Runtime::make_symbol(std::optional<std::string> description) {
    const SymbolId id = static_cast<SymbolId>(symbols_.size());
    auto record = std::make_unique<detail::SymbolRecord>();
    record->owner = this;
    record->id = id;
    record->description = std::move(description);
    const auto* raw = record.get();
    symbols_.push_back(std::move(record));
    return Value(raw);
}

Value Runtime::symbol_for(std::string_view key) {
    const auto found = symbol_registry_.find(std::string(key));
    if (found != symbol_registry_.end()) return Value(symbols_[found->second].get());
    Value value = make_symbol(std::string(key));
    symbols_[value.as_symbol_id()]->registry_key = std::string(key);
    symbol_registry_.emplace(std::string(key), value.as_symbol_id());
    return value;
}

std::optional<std::string_view> Runtime::symbol_key_for(Value symbol) const noexcept {
    if (!symbol.is_symbol() || symbol.owning_runtime() != this) return std::nullopt;
    const auto* record = symbols_[symbol.as_symbol_id()].get();
    if (!record->registry_key) return std::nullopt;
    return std::string_view(*record->registry_key);
}

Value Runtime::well_known_symbol(std::string_view name) {
    const auto found = well_known_symbols_.find(std::string(name));
    if (found != well_known_symbols_.end()) return Value(symbols_[found->second].get());
    Value value = make_symbol(std::string("Symbol.") + std::string(name));
    symbols_[value.as_symbol_id()]->well_known_name = std::string(name);
    well_known_symbols_.emplace(std::string(name), value.as_symbol_id());
    return value;
}

Realm& Runtime::create_realm() {
    std::unique_ptr<Realm> realm(new Realm(*this));
    Realm* raw = realm.get();
    realms_.push_back(std::move(realm));
    raw->global_object_ = make_object(*raw);
    return *raw;
}

Value Runtime::make_object(Realm& realm) {
    assert(&realm.runtime() == this);
    auto cell = std::make_unique<detail::HeapObject>(this, &realm, false);
    auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return Value(raw);
}

Value Runtime::make_array(Realm& realm) {
    assert(&realm.runtime() == this);
    auto cell = std::make_unique<detail::HeapObject>(this, &realm, true);
    cell->object_kind = ObjectKind::Array;
    auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return Value(raw);
}

Value Runtime::make_promise(Realm& realm) {
    assert(&realm.runtime() == this);
    auto cell = std::make_unique<detail::HeapObject>(this, &realm, false, true);
    cell->object_kind = ObjectKind::Promise;
    auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return Value(raw);
}

Value Runtime::make_function(Realm& realm, std::string name, std::uint32_t function_length, bytecode::BytecodeChunk chunk, ConstructorKind constructor_kind, bool generator, std::optional<std::uint32_t> arguments_slot, ThisMode this_mode, bool method, std::optional<std::uint32_t> parameter_count, bool simple_parameter_list, bool strict, std::vector<std::string> parameter_names) {
    auto code = std::make_shared<detail::FunctionCode>(std::move(name), function_length, std::move(chunk), constructor_kind, generator, arguments_slot, this_mode, method, parameter_count, simple_parameter_list, strict, std::move(parameter_names));
    assert(&realm.runtime() == this);
    auto cell = std::make_unique<detail::HeapFunction>(this, &realm, std::move(code));
    if (!realm.function_prototype_.is_undefined()) cell->prototype = realm.function_prototype_;
    const auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return Value(raw);
}

Value Runtime::make_native_function(Realm& realm, std::string name, std::uint32_t arity, NativeFunction function, ConstructorKind constructor_kind) {
    assert(function != nullptr);
    auto code = std::make_shared<detail::FunctionCode>(std::move(name), arity, function, constructor_kind);
    assert(&realm.runtime() == this);
    auto cell = std::make_unique<detail::HeapFunction>(this, &realm, std::move(code));
    if (!realm.function_prototype_.is_undefined()) cell->prototype = realm.function_prototype_;
    const auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return Value(raw);
}

Value Runtime::make_bound_function(Realm& realm, Value target, Value bound_this, std::span<const Value> bound_arguments) {
    assert(target.is_function());
    const auto* target_function = target.as_heap_function();
    const std::uint32_t bound_count = static_cast<std::uint32_t>(std::min<std::size_t>(bound_arguments.size(), target_function->code->arity));
    const std::uint32_t arity = target_function->code->arity - bound_count;
    auto code = std::make_shared<detail::FunctionCode>("bound " + target_function->code->name, arity);
    code->constructor_kind = target_function->code->constructor_kind;
    code->this_mode = target_function->code->this_mode;
    auto cell = std::make_unique<detail::HeapFunction>(this, &realm, std::move(code));
    cell->prototype = realm.function_prototype_;
    cell->bound_target = target;
    cell->bound_this = bound_this;
    cell->bound_arguments.assign(bound_arguments.begin(), bound_arguments.end());
    auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return Value(raw);
}

Value Runtime::make_closure(const detail::HeapFunction& prototype, std::vector<detail::HeapUpvalue*> upvalues, detail::HeapModuleEnvironment* module_environment) {
    auto cell = std::make_unique<detail::HeapFunction>(this, prototype.realm, prototype.code, std::move(upvalues), module_environment);
    cell->prototype = prototype.prototype;
    cell->home_object = prototype.home_object;
    auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    const Value closure(raw);

    // Ordinary constructible ECMAScript functions receive a fresh prototype
    // object for each function-object evaluation. Methods, arrows, generators,
    // and bound functions are deliberately excluded.
    if (prototype.code->constructor_kind != ConstructorKind::None && !prototype.code->method) {
        Value instance_prototype = make_object(*prototype.realm);
        auto* instance_object = instance_prototype.as_heap_object();
        instance_object->prototype = prototype.realm->object_prototype_;

        const PropertyKey prototype_key = PropertyKey::atom(intern_atom("prototype"));
        raw->properties.emplace(prototype_key, PropertyDescriptor::data(instance_prototype, false, false, false));
        raw->property_order.push_back(prototype_key);

        const PropertyKey constructor_key = PropertyKey::atom(intern_atom("constructor"));
        instance_object->properties.emplace(constructor_key, PropertyDescriptor::data(closure, true, false, true));
        instance_object->property_order.push_back(constructor_key);
    }
    return closure;
}

detail::HeapModuleEnvironment* Runtime::make_module_environment(std::size_t binding_count) {
    auto cell = std::make_unique<detail::HeapModuleEnvironment>(this, binding_count);
    auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return raw;
}

detail::HeapUpvalue* Runtime::make_upvalue(BindingSlot* location) {
    assert(location != nullptr);
    auto cell = std::make_unique<detail::HeapUpvalue>(this, location);
    auto* raw = cell.get();
    heap_.push_back(std::move(cell));
    return raw;
}

bool Runtime::owns(const Value& value) const noexcept {
    const Runtime* owner = value.owning_runtime();
    return owner == nullptr || owner == this;
}

void Runtime::register_context() noexcept {
    ++active_contexts_;
}

void Runtime::unregister_context() noexcept {
    assert(active_contexts_ > 0);
    --active_contexts_;
}

void Runtime::register_vm(VM& vm) {
    assert(std::find(active_vms_.begin(), active_vms_.end(), &vm) == active_vms_.end());
    active_vms_.push_back(&vm);
}

void Runtime::register_module_environment(detail::HeapModuleEnvironment& environment) {
    assert(environment.owner == this);
    assert(std::find(module_environments_.begin(), module_environments_.end(), &environment) == module_environments_.end());
    module_environments_.push_back(&environment);
}

void Runtime::unregister_module_environment(detail::HeapModuleEnvironment& environment) noexcept {
    const auto found = std::find(module_environments_.begin(), module_environments_.end(), &environment);
    assert(found != module_environments_.end());
    if (found != module_environments_.end()) module_environments_.erase(found);
}

void Runtime::unregister_vm(VM& vm) noexcept {
    const auto found = std::find(active_vms_.begin(), active_vms_.end(), &vm);
    assert(found != active_vms_.end());
    if (found != active_vms_.end()) active_vms_.erase(found);
}

Value Runtime::persistent_root_value(std::uint64_t id) const noexcept {
    const auto found = persistent_roots_.find(id);
    if (found == persistent_roots_.end()) return Value::undefined();
    return found->second;
}

Result<void> Runtime::set_persistent_root(std::uint64_t id, Value value) {
    if (!owns(value)) return Error{ErrorCode::runtime_mismatch, "persistent root Value belongs to a different Runtime"};
    const auto found = persistent_roots_.find(id);
    if (found == persistent_roots_.end()) return Error{ErrorCode::internal, "persistent root is no longer registered"};
    found->second = value;
    return {};
}

void Runtime::release_persistent_root(std::uint64_t id) noexcept {
    persistent_roots_.erase(id);
}

void Runtime::release_bytecode_root(std::uint64_t id) noexcept {
    bytecode_roots_.erase(id);
}

void Runtime::mark_value(Value value) {
    switch (value.tag_) {
    case ValueTag::string:
        mark_cell(const_cast<detail::HeapString*>(value.payload_.string));
        break;
    case ValueTag::symbol:
        break; // Runtime-owned immortal symbol records are not GC cells.
    case ValueTag::function:
        mark_cell(const_cast<detail::HeapFunction*>(value.payload_.function));
        break;
    case ValueTag::object:
        mark_cell(value.payload_.object);
        break;
    case ValueTag::undefined:
    case ValueTag::null:
    case ValueTag::boolean:
    case ValueTag::number:
        break;
    }
}

void Runtime::mark_chunk(const bytecode::BytecodeChunk& chunk) {
    for (const Value constant : chunk.constants()) mark_value(constant);
}

void Runtime::mark_cell(detail::HeapCell* cell) {
    if (cell == nullptr || cell->marked) return;
    assert(cell->owner == this);
    cell->marked = true;
    mark_stack_.push_back(cell);
}

void Runtime::trace_cell(detail::HeapCell* cell) {
    switch (cell->kind) {
    case detail::HeapKind::string:
        break;
    case detail::HeapKind::function: {
        auto* function = static_cast<detail::HeapFunction*>(cell);
        for (auto* upvalue : function->upvalues) mark_cell(upvalue);
        mark_cell(function->module_environment);
        if (function->lexical_this) mark_value(*function->lexical_this);
        mark_value(function->home_object);
        mark_value(function->bound_target);
        mark_value(function->bound_this);
        for (Value value : function->bound_arguments) mark_value(value);
        mark_value(function->prototype);
        for (const auto& [key, property] : function->properties) {
            (void)key;
            if (property.value) mark_value(*property.value);
            if (property.get) mark_value(*property.get);
            if (property.set) mark_value(*property.set);
        }
        if (!function->code->is_native()) mark_chunk(function->code->chunk);
        break;
    }
    case detail::HeapKind::upvalue: {
        auto* upvalue = static_cast<detail::HeapUpvalue*>(cell);
        mark_value(upvalue->get());
        break;
    }
    case detail::HeapKind::module_environment: {
        auto* environment = static_cast<detail::HeapModuleEnvironment*>(cell);
        for (const auto& binding : environment->bindings) {
            mark_cell(binding.local_upvalue);
            mark_cell(binding.target_environment);
        }
        break;
    }
    case detail::HeapKind::object: {
        auto* object = static_cast<detail::HeapObject*>(cell);
        mark_value(object->prototype);
        if (object->is_generator) {
            const auto& generator = object->generator_state;
            mark_value(generator.this_value);
            for (const BindingSlot& slot : generator.locals) mark_value(slot.value);
            for (Value value : generator.actual_arguments) mark_value(value);
            for (Value value : generator.stack) mark_value(value);
            for (auto* upvalue : generator.upvalues) mark_cell(upvalue);
            for (auto* upvalue : generator.captured_locals) mark_cell(upvalue);
            mark_cell(generator.module_environment);
            if (generator.code && !generator.code->is_native()) mark_chunk(generator.code->chunk);
        }
        if (object->boxed_primitive) mark_value(*object->boxed_primitive);
        for (auto* upvalue : object->mapped_argument_upvalues) mark_cell(upvalue);
        if (object->is_promise) {
            mark_value(object->promise_result);
            for (const auto& reaction : object->promise_reactions) {
                mark_value(reaction.on_fulfilled);
                mark_value(reaction.on_rejected);
                mark_value(reaction.next_promise);
            }
        }
        for (const auto& element : object->elements) if (element) mark_value(*element);
        for (const auto& [key, property] : object->properties) {
            (void)key;
            if (property.value) mark_value(*property.value);
            if (property.get) mark_value(*property.get);
            if (property.set) mark_value(*property.set);
        }
        break;
    }
    }
}

void Runtime::mark_vm(const VM& vm) {
    mark_value(vm.active_generator_);
    for (const Value value : vm.stack_) mark_value(value);
    for (const auto& frame : vm.frames_) {
        mark_chunk(*frame.chunk);
        mark_value(frame.this_value);
        for (const BindingSlot& local : frame.locals) mark_value(local.value);
        for (const Value argument : frame.actual_arguments) mark_value(argument);
        for (auto* upvalue : frame.upvalues) mark_cell(upvalue);
        for (auto* upvalue : frame.captured_locals) mark_cell(upvalue);
        for (const auto& pending : frame.pending_completions) mark_value(pending.completion.value());
        if (frame.construct_receiver) mark_value(*frame.construct_receiver);
        mark_cell(frame.execution_context.module_environment);
    }
}

void Runtime::mark_roots() {
    for (const auto& realm : realms_) {
        mark_value(realm->global_object_);
        mark_value(realm->object_prototype_);
        mark_value(realm->array_prototype_);
        mark_value(realm->promise_prototype_);
        mark_value(realm->regexp_prototype_);
        for (const Value value : realm->global_environment_.binding_values()) mark_value(value);
    }
    for (const auto& [id, value] : persistent_roots_) {
        (void)id;
        mark_value(value);
    }
    for (const auto& [id, chunk] : bytecode_roots_) {
        (void)id;
        mark_chunk(*chunk);
    }
    for (const VM* vm : active_vms_) mark_vm(*vm);
    for (auto* environment : module_environments_) mark_cell(environment);
    for (const auto& job : jobs_) {
        mark_value(job.result);
        mark_value(job.on_fulfilled);
        mark_value(job.on_rejected);
        mark_value(job.next_promise);
    }
}

void Runtime::enqueue_promise_reaction(bool fulfilled, Value result, Value on_fulfilled, Value on_rejected, Value next_promise) {
    jobs_.push_back(PromiseReactionJob{fulfilled, result, on_fulfilled, on_rejected, next_promise});
}

Result<void> Runtime::settle_promise(Value promise, bool fulfilled, Value result) {
    if (!owns(promise) || !owns(result)) return Error{ErrorCode::runtime_mismatch, "promise settlement Value belongs to a different Runtime"};
    if (!promise.is_object() || !promise.as_heap_object()->is_promise) return Error{ErrorCode::type_error, "promise settlement target is not a Promise"};
    auto* target = promise.as_heap_object();
    if (target->promise_state != detail::PromiseState::pending) return {};

    if (fulfilled && result.is_object() && result.as_heap_object()->is_promise) {
        if (result.as_heap_object() == target) return Error{ErrorCode::type_error, "Promise cannot resolve with itself"};
        auto* source = result.as_heap_object();
        if (source->promise_state == detail::PromiseState::pending) {
            source->promise_reactions.push_back(detail::PromiseReaction{Value::undefined(), Value::undefined(), promise});
            return {};
        }
        const bool source_fulfilled = source->promise_state == detail::PromiseState::fulfilled;
        enqueue_promise_reaction(source_fulfilled, source->promise_result, Value::undefined(), Value::undefined(), promise);
        return {};
    }

    target->promise_state = fulfilled ? detail::PromiseState::fulfilled : detail::PromiseState::rejected;
    target->promise_result = result;
    for (const auto& reaction : target->promise_reactions) {
        enqueue_promise_reaction(fulfilled, result, reaction.on_fulfilled, reaction.on_rejected, reaction.next_promise);
    }
    target->promise_reactions.clear();
    return {};
}

Result<std::size_t> Runtime::run_jobs(Context& context) {
    if (&context.runtime() != this) return Error{ErrorCode::runtime_mismatch, "job Context belongs to a different Runtime"};
    std::size_t ran = 0;
    while (!jobs_.empty()) {
        const PromiseReactionJob job = jobs_.front();
        ++ran;

        const Value handler = job.fulfilled ? job.on_fulfilled : job.on_rejected;
        if (handler.is_undefined()) {
            const auto propagated = settle_promise(job.next_promise, job.fulfilled, job.result);
            if (!propagated) return propagated.error();
            jobs_.pop_front();
            continue;
        }
        if (!handler.is_function()) return Error{ErrorCode::type_error, "Promise reaction handler is not a function"};

        VM vm(context);
        const Value argument = job.result;
        const auto callback_result = vm.call(handler, std::span<const Value>(&argument, 1U));
        if (!callback_result) return callback_result.error().legacy_error();
        if (callback_result.completion().is_throw()) {
            const auto rejected = settle_promise(job.next_promise, false, callback_result.completion().value());
            if (!rejected) return rejected.error();
            jobs_.pop_front();
            continue;
        }
        if (!callback_result.completion().is_normal()) {
            return Error{ErrorCode::internal, "Promise reaction escaped with non-call completion"};
        }
        const auto resolved = settle_promise(job.next_promise, true, callback_result.completion().value());
        if (!resolved) return resolved.error();
        jobs_.pop_front();
    }
    return ran;
}

GarbageCollectionStats Runtime::collect_garbage() {
    GarbageCollectionStats stats;
    stats.before = heap_.size();

    mark_stack_.clear();
    mark_roots();
    while (!mark_stack_.empty()) {
        detail::HeapCell* cell = mark_stack_.back();
        mark_stack_.pop_back();
        trace_cell(cell);
    }

    for (const auto& cell : heap_) {
        if (cell->marked) ++stats.marked;
    }

    const auto new_end = std::remove_if(heap_.begin(), heap_.end(), [](const std::unique_ptr<detail::HeapCell>& cell) {
        if (!cell->marked) return true;
        cell->marked = false;
        return false;
    });
    heap_.erase(new_end, heap_.end());

    stats.after = heap_.size();
    stats.collected = stats.before - stats.after;
    ++collection_count_;
    return stats;
}

} // namespace js
