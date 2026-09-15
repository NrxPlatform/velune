#include <js/vm/vm.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "detail/heap.hpp"
#include <js/abstract_operations.hpp>
#include <js/bytecode/builder.hpp>
#include <js/bytecode/opcode.hpp>
#include <js/bytecode/verifier.hpp>
#include <js/error.hpp>

namespace js {
namespace {

template <typename Function>
class ScopeExit final {
public:
    explicit ScopeExit(Function function) : function_(std::move(function)) {}
    ~ScopeExit() { function_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
private:
    Function function_;
};

[[nodiscard]] std::uint32_t read_u32(const std::vector<std::uint8_t>& code, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(code[offset]) |
           (static_cast<std::uint32_t>(code[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(code[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(code[offset + 3]) << 24U);
}


[[nodiscard]] std::vector<BindingSlot> make_local_slots(const bytecode::BytecodeChunk& chunk) {
    const std::size_t count = static_cast<std::size_t>(chunk.local_count());
    std::vector<BindingSlot> slots(count);
    const auto& states = chunk.local_binding_states();
    const auto& immutable = chunk.local_binding_immutable();
    for (std::size_t index = 0; index < count; ++index) {
        slots[index].state = states.empty() ? BindingState::InitializedMutable : states[index];
        slots[index].immutable = !immutable.empty() && immutable[index];
    }
    return slots;
}

[[nodiscard]] Result<Value> get_binding_slot(const BindingSlot& slot) {
    if (!slot.initialized()) return Error{ErrorCode::reference_error, "cannot access lexical binding before initialization"};
    return slot.value;
}

[[nodiscard]] Result<void> set_binding_slot(BindingSlot& slot, Value value) {
    if (!slot.initialized()) return Error{ErrorCode::reference_error, "cannot assign to lexical binding before initialization"};
    if (!slot.mutable_binding()) return Error{ErrorCode::type_error, "assignment to immutable binding"};
    slot.value = value;
    return {};
}

[[nodiscard]] Result<void> initialize_binding_slot(BindingSlot& slot, Value value) {
    if (slot.initialized()) return Error{ErrorCode::internal, "binding initialized more than once"};
    slot.value = value;
    slot.state = slot.immutable ? BindingState::InitializedImmutable : BindingState::InitializedMutable;
    return {};
}

ExecutionResult generator_next_builtin(Context& context, Value receiver, std::span<const Value>) {
    return context.resume_generator(receiver);
}

ExecutionResult generator_iterator_builtin(Context&, Value receiver, std::span<const Value>) {
    return Completion::normal(receiver);
}

ExecutionResult arguments_iterator_builtin(Context& context, Value receiver, std::span<const Value>) {
    return execution_from_result(context.array_iterator(receiver));
}

ExecutionResult arguments_callee_thrower(Context& context, Value, std::span<const Value>) {
    return Completion::throw_(context.string("TypeError: access to strict arguments.callee is forbidden"));
}


} // namespace

void VM::reset() {
    close_all_open_upvalues();
    stack_.clear();
    frames_.clear();
    root_local_count_ = 0;
    maximum_frame_depth_ = 0;
    generator_suspended_ = false;
    active_generator_ = Value::undefined();
}

Result<Value> VM::pop_number_operand(const char* operation_name, std::size_t stack_base) {
    if (stack_.size() <= stack_base) return Error{ErrorCode::vm_error, std::string(operation_name) + ": operand stack underflow in current frame"};
    Value value = stack_.back();
    stack_.pop_back();
    if (!value.is_number()) return Error{ErrorCode::type_error, std::string(operation_name) + ": numeric opcode received non-number operand"};
    return value;
}

detail::HeapUpvalue* VM::capture_local(Frame& frame, std::uint32_t local_index) {
    const std::size_t index = static_cast<std::size_t>(local_index);
    if (frame.captured_locals[index] != nullptr) return frame.captured_locals[index];
    auto* upvalue = context_->runtime().make_upvalue(&frame.locals[index]);
    frame.captured_locals[index] = upvalue;
    return upvalue;
}

void VM::close_frame_upvalues(Frame& frame) noexcept {
    for (auto*& upvalue : frame.captured_locals) {
        if (upvalue == nullptr) continue;
        upvalue->close();
        upvalue = nullptr;
    }
}

void VM::close_all_open_upvalues() noexcept {
    for (auto& frame : frames_) close_frame_upvalues(frame);
}

void VM::discard_overridden_completions(Frame& frame, std::size_t pc) noexcept {
    while (!frame.pending_completions.empty()) {
        const auto finally_start = frame.pending_completions.back().finally_start;
        const bytecode::ExceptionHandler* owner = nullptr;
        for (const auto& handler : frame.chunk->exception_handlers()) {
            if (handler.finally_start == finally_start) { owner = &handler; break; }
        }
        if (owner == nullptr || owner->finally_end == bytecode::no_handler_target ||
            pc < owner->finally_start || pc >= owner->finally_end) break;
        frame.pending_completions.pop_back();
    }
}

const bytecode::ExceptionHandler* VM::find_exception_handler(const Frame& frame, std::size_t pc, bool& use_catch) const noexcept {
    const bytecode::ExceptionHandler* best = nullptr;
    std::size_t best_span = static_cast<std::size_t>(-1);
    bool best_catch = false;
    for (const auto& handler : frame.chunk->exception_handlers()) {
        bool candidate = false;
        bool candidate_catch = false;
        if (pc >= handler.try_start && pc < handler.try_end) {
            if (handler.catch_start != bytecode::no_handler_target) { candidate = true; candidate_catch = true; }
            else if (handler.finally_start != bytecode::no_handler_target) candidate = true;
        } else if (handler.catch_start != bytecode::no_handler_target && handler.catch_end != bytecode::no_handler_target &&
                   pc >= handler.catch_start && pc < handler.catch_end && handler.finally_start != bytecode::no_handler_target) {
            candidate = true;
        }
        if (!candidate) continue;
        const std::size_t region_end = handler.catch_end != bytecode::no_handler_target ? handler.catch_end : handler.try_end;
        const std::size_t span = region_end - handler.try_start;
        if (best == nullptr || span < best_span) { best = &handler; best_span = span; best_catch = candidate_catch; }
    }
    use_catch = best_catch;
    return best;
}

const bytecode::ExceptionHandler* VM::find_finally_handler(const Frame& frame, std::size_t pc) const noexcept {
    const bytecode::ExceptionHandler* best = nullptr;
    std::size_t best_span = static_cast<std::size_t>(-1);
    for (const auto& handler : frame.chunk->exception_handlers()) {
        if (handler.finally_start == bytecode::no_handler_target) continue;
        const bool in_try = pc >= handler.try_start && pc < handler.try_end;
        const bool in_catch = handler.catch_start != bytecode::no_handler_target && handler.catch_end != bytecode::no_handler_target &&
                              pc >= handler.catch_start && pc < handler.catch_end;
        if (!in_try && !in_catch) continue;
        const std::size_t region_end = handler.catch_end != bytecode::no_handler_target ? handler.catch_end : handler.try_end;
        const std::size_t span = region_end - handler.try_start;
        if (best == nullptr || span < best_span) { best = &handler; best_span = span; }
    }
    return best;
}

std::optional<ExecutionResult> VM::propagate_completion(Completion completion, std::size_t origin_pc, std::size_t boundary_depth) {
    std::size_t pc = origin_pc;

    if (completion.is_return()) {
        if (frames_.size() <= boundary_depth) {
            return ExecutionResult{EngineFailure{EngineFailureCode::InternalInvariant, "return completion crossed invocation boundary without a frame"}};
        }
        Frame& frame = frames_.back();
        discard_overridden_completions(frame, pc);
        if (const auto* handler = find_finally_handler(frame, pc)) {
            stack_.resize(frame.stack_base);
            frame.pending_completions.push_back(PendingFinally{completion, handler->finally_start});
            frame.pc = handler->finally_start;
            return std::nullopt;
        }

        Value value = completion.value();
        if (frame.construct_receiver && !value.is_object_like()) value = *frame.construct_receiver;
        const std::size_t return_base = frame.stack_base;
        close_frame_upvalues(frame);
        stack_.resize(return_base);
        frames_.pop_back();
        if (frames_.size() == boundary_depth) return ExecutionResult{Completion::normal(value)};
        stack_.push_back(value);
        return std::nullopt;
    }

    if (completion.type() == CompletionType::Break || completion.type() == CompletionType::Continue) {
        if (frames_.size() <= boundary_depth) {
            return ExecutionResult{EngineFailure{EngineFailureCode::InternalInvariant, "loop completion crossed invocation boundary"}};
        }
        Frame& frame = frames_.back();
        discard_overridden_completions(frame, pc);
        if (const auto* handler = find_finally_handler(frame, pc)) {
            stack_.resize(frame.stack_base);
            frame.pending_completions.push_back(PendingFinally{completion, handler->finally_start});
            frame.pc = handler->finally_start;
            return std::nullopt;
        }
        frame.pc = static_cast<std::size_t>(completion.target());
        return std::nullopt;
    }

    if (!completion.is_throw()) return ExecutionResult{completion};

    const Value exception = completion.value();
    while (frames_.size() > boundary_depth) {
        Frame& frame = frames_.back();
        discard_overridden_completions(frame, pc);
        bool use_catch = false;
        const auto* handler = find_exception_handler(frame, pc, use_catch);
        if (handler != nullptr) {
            stack_.resize(frame.stack_base);
            if (use_catch) {
                stack_.push_back(exception);
                frame.pc = handler->catch_start;
            } else {
                frame.pending_completions.push_back(PendingFinally{completion, handler->finally_start});
                frame.pc = handler->finally_start;
            }
            return std::nullopt;
        }

        const std::size_t base = frame.stack_base;
        close_frame_upvalues(frame);
        stack_.resize(base);
        frames_.pop_back();
        if (frames_.size() == boundary_depth) return ExecutionResult{completion};
        pc = frames_.back().last_instruction_pc;
    }
    return ExecutionResult{completion};
}

ExecutionResult VM::invoke_function(Value callee, std::uint32_t argument_count, std::size_t operand_base, std::size_t argument_start, Value this_value, bool& frame_pushed, std::optional<Value> construct_receiver) {
    frame_pushed = false;
    const auto validation = context_->validate(callee);
    if (!validation) return validation.error();
    const auto this_validation = context_->validate(this_value);
    if (!this_validation) return this_validation.error();
    if (!callee.is_function()) return Error{ErrorCode::type_error, "call target is not a function"};

    const auto* function = callee.as_heap_function();
    if (function->code->this_mode == ThisMode::Lexical && function->lexical_this) {
        this_value = *function->lexical_this;
    } else if (function->code->this_mode == ThisMode::Global) {
        if (this_value.is_undefined() || this_value.is_null()) this_value = function->realm->global_object();
        else if (!this_value.is_object_like()) this_value = context_->box_primitive(this_value);
    }
    if (function->code->method && function->code->constructor_kind != ConstructorKind::None && !construct_receiver) {
        return Error{ErrorCode::type_error, "class constructor '" + function->code->name + "' cannot be called without new"};
    }
    const std::size_t argc = static_cast<std::size_t>(argument_count);
    if (argument_start > stack_.size() || stack_.size() - argument_start < argc || operand_base > argument_start) {
        return Error{ErrorCode::vm_error, "call operand layout is invalid"};
    }

    std::vector<Value> arguments;
    arguments.reserve(argc);
    for (std::size_t index = 0; index < argc; ++index) arguments.push_back(stack_[argument_start + index]);

    if (!function->bound_target.is_undefined()) {
        std::vector<Value> combined = function->bound_arguments;
        combined.insert(combined.end(), arguments.begin(), arguments.end());
        const auto result = invoke(function->bound_target, function->bound_this, combined);
        if (!result) return result.error();
        if (!result.completion().is_normal()) return result.completion();
        stack_.resize(operand_base);
        stack_.push_back(result.completion().value());
        return Completion::normal(result.completion().value());
    }

    auto initialize_call_locals = [&](std::vector<BindingSlot>& locals, std::vector<detail::HeapUpvalue*>& captured_locals) -> Result<void> {
        if (locals.empty()) return Error{ErrorCode::vm_error, "compiled function frame has no self slot"};
        if (captured_locals.size() != locals.size()) captured_locals.assign(locals.size(), nullptr);
        locals[0].value = callee;
        const std::size_t parameter_count = static_cast<std::size_t>(function->code->parameter_count);
        const std::size_t supplied_parameters = std::min(argc, parameter_count);
        if (function->code->simple_parameter_list) {
            for (std::size_t index = 0; index < supplied_parameters; ++index) {
                if (index + 1U >= locals.size()) return Error{ErrorCode::vm_error, "compiled parameter slots exceed local frame"};
                locals[index + 1U].value = arguments[index];
            }
        }
        if (function->code->arguments_slot) {
            const std::size_t slot = static_cast<std::size_t>(*function->code->arguments_slot);
            if (slot >= locals.size()) return Error{ErrorCode::vm_error, "arguments binding slot exceeds local frame"};
            Value arguments_object = context_->object_in_realm(*function->realm);
            auto* arguments_heap = arguments_object.as_heap_object();
            const bool mapped = function->code->simple_parameter_list && !function->code->strict && function->code->this_mode != ThisMode::Lexical;
            if (mapped) {
                arguments_heap->object_kind = ObjectKind::MappedArguments;
                arguments_heap->mapped_argument_upvalues.resize(arguments.size(), nullptr);
            } else {
                arguments_heap->object_kind = ObjectKind::Arguments;
            }
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                const auto set = context_->set_own_property(arguments_object, std::to_string(index), arguments[index]);
                if (!set) return set.error();
                if (mapped && index < parameter_count && index + 1U < locals.size()) {
                    bool shadowed_by_later_duplicate = false;
                    if (index < function->code->parameter_names.size()) {
                        for (std::size_t later = index + 1U; later < function->code->parameter_names.size(); ++later) {
                            if (function->code->parameter_names[index] == function->code->parameter_names[later]) {
                                shadowed_by_later_duplicate = true;
                                break;
                            }
                        }
                    }
                    if (!shadowed_by_later_duplicate) {
                        auto*& upvalue = captured_locals[index + 1U];
                        if (upvalue == nullptr) upvalue = context_->runtime().make_upvalue(&locals[index + 1U]);
                        arguments_heap->mapped_argument_upvalues[index] = upvalue;
                    }
                }
            }
            const auto length = context_->define_own_property(arguments_object, "length",
                PropertyDescriptor::data(Value::number(static_cast<double>(arguments.size())), true, false, true));
            if (!length) return length.error();
            if (!*length) return Error{ErrorCode::internal, "failed to define arguments.length"};

            if (function->code->strict) {
                const Value thrower = context_->native_function_in_realm(*function->realm, "ThrowTypeError", 0, arguments_callee_thrower);
                const auto callee_property = context_->define_own_property(arguments_object, "callee",
                    PropertyDescriptor::accessor(thrower, thrower, false, false));
                if (!callee_property) return callee_property.error();
                if (!*callee_property) return Error{ErrorCode::internal, "failed to define strict arguments.callee"};
            } else {
                const auto callee_property = context_->define_own_property(arguments_object, "callee",
                    PropertyDescriptor::data(callee, true, false, true));
                if (!callee_property) return callee_property.error();
                if (!*callee_property) return Error{ErrorCode::internal, "failed to define arguments.callee"};
            }

            const auto iterator = context_->define_own_property(arguments_object,
                PropertyKey::symbol(context_->well_known_symbol("iterator").as_symbol_id()),
                PropertyDescriptor::data(context_->native_function_in_realm(*function->realm, "arguments[Symbol.iterator]", 0, arguments_iterator_builtin), true, false, true));
            if (!iterator) return iterator.error();
            if (!*iterator) return Error{ErrorCode::internal, "failed to define arguments iterator"};
            locals[slot].value = arguments_object;
        }
        return {};
    };

    if (function->code->generator) {
        if (construct_receiver) return Error{ErrorCode::type_error, "generator functions are not constructors"};
        Value generator = context_->object_in_realm(*function->realm);
        auto* object = generator.as_heap_object();
        object->is_generator = true;
        object->object_kind = ObjectKind::Generator;
        auto& state = object->generator_state;
        state.realm = function->realm;
        state.function = function;
        state.code = function->code;
        state.this_value = this_value;
        state.upvalues = function->upvalues;
        state.module_environment = function->module_environment;
        state.locals = make_local_slots(function->code->chunk);
        state.captured_locals.assign(state.locals.size(), nullptr);
        const auto initialized = initialize_call_locals(state.locals, state.captured_locals); if (!initialized) return initialized.error();
        state.actual_arguments = arguments;
        const auto next = context_->set_own_property(generator, "next", context_->native_function_in_realm(*function->realm, "Generator.next", 0, generator_next_builtin));
        if (!next) return next.error();
        const auto iterator = context_->set_own_property(generator, PropertyKey::symbol(context_->well_known_symbol("iterator").as_symbol_id()), context_->native_function_in_realm(*function->realm, "Generator[Symbol.iterator]", 0, generator_iterator_builtin));
        if (!iterator) return iterator.error();
        stack_.resize(operand_base);
        stack_.push_back(generator);
        return Completion::normal(generator);
    }

    if (function->code->is_native()) {
        Realm* previous_execution_realm = context_->execution_realm_;
        context_->execution_realm_ = function->realm;
        ScopeExit realm_cleanup([this, previous_execution_realm] { context_->execution_realm_ = previous_execution_realm; });
        const auto native_result = function->code->native(*context_, this_value, std::span<const Value>(arguments));
        if (!native_result) return native_result.error();
        if (native_result.completion().is_throw()) return native_result.completion();
        if (!native_result.completion().is_normal()) return EngineFailure{EngineFailureCode::HostContractViolation, "native function returned non-call completion"};
        const Value native_value = native_result.completion().value();
        const auto result_validation = context_->validate(native_value);
        if (!result_validation) return result_validation.error();
        stack_.resize(operand_base);
        const Value result = construct_receiver && !native_value.is_object_like() ? *construct_receiver : native_value;
        stack_.push_back(result);
        return Completion::normal(result);
    }

    if (function->upvalues.size() != static_cast<std::size_t>(function->code->chunk.upvalue_count())) {
        return Error{ErrorCode::vm_error, "function closure upvalue count does not match compiled function metadata"};
    }

    bytecode::BytecodeVerifier verifier;
    const auto verification = verifier.verify(function->code->chunk);
    if (!verification) return verification.error();

    stack_.resize(operand_base);
    std::vector<BindingSlot> locals = make_local_slots(function->code->chunk);
    std::vector<detail::HeapUpvalue*> captured_locals(locals.size(), nullptr);
    const auto initialized = initialize_call_locals(locals, captured_locals); if (!initialized) return initialized.error();
    frames_.push_back(Frame{&function->code->chunk, 0U, 0U, operand_base, this_value, std::move(locals), arguments, function->upvalues, std::move(captured_locals), {}, construct_receiver, ExecutionContext{function->realm, function, function->module_environment}});
    if (frames_.size() > maximum_frame_depth_) maximum_frame_depth_ = frames_.size();
    frame_pushed = true;
    return Completion::normal(Value::undefined());
}

ExecutionResult VM::invoke_construct(Value callee, std::uint32_t argument_count, std::size_t operand_base, std::size_t argument_start, bool& frame_pushed) {
    const auto validation = context_->validate(callee);
    if (!validation) return validation.error();
    if (!callee.is_function()) return Error{ErrorCode::type_error, "constructor target is not a function"};
    const auto* function = callee.as_heap_function();
    if (!function->bound_target.is_undefined()) {
        const std::size_t argc = static_cast<std::size_t>(argument_count);
        std::vector<Value> combined = function->bound_arguments;
        combined.reserve(combined.size() + argc);
        for (std::size_t i = 0; i < argc; ++i) combined.push_back(stack_[argument_start + i]);
        const auto result = construct(function->bound_target, combined);
        if (!result) return result.error();
        if (!result.completion().is_normal()) return result.completion();
        stack_.resize(operand_base);
        stack_.push_back(result.completion().value());
        return Completion::normal(result.completion().value());
    }
    if (function->code->constructor_kind == ConstructorKind::None)
        return Error{ErrorCode::type_error, "function is not a constructor"};

    Value prototype = function->realm->object_prototype();
    const auto prototype_property = context_->get_property_semantic(callee, context_->property_key("prototype"));
    if (!prototype_property) return prototype_property.error();
    if (prototype_property.completion().is_throw()) return prototype_property.completion();
    if (!prototype_property.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "constructor prototype lookup produced non-normal completion"};
    if (prototype_property.completion().value().is_object_like()) prototype = prototype_property.completion().value();

    const auto receiver_result = context_->object_in_realm(*callee.as_heap_function()->realm, prototype);
    if (!receiver_result) return receiver_result.error();
    const Value receiver = *receiver_result;
    return invoke_function(callee, argument_count, operand_base, argument_start, receiver, frame_pushed, receiver);
}

ExecutionResult VM::run(const bytecode::BytecodeChunk& chunk) {
    return run_impl(chunk, nullptr);
}

ExecutionResult VM::call(Value callee, std::span<const Value> arguments, Value this_value) {
    return invoke(callee, this_value, arguments);
}

ExecutionResult VM::invoke(Value callee, Value this_value, std::span<const Value> arguments) {
    const auto callee_validation = context_->validate(callee);
    if (!callee_validation) return callee_validation.error();
    const auto this_validation = context_->validate(this_value);
    if (!this_validation) return this_validation.error();
    for (const Value argument : arguments) {
        const auto validation = context_->validate(argument);
        if (!validation) return validation.error();
    }

    const bool owns_execution_boundary = !registered_;
    Runtime& runtime = context_->runtime();
    if (owns_execution_boundary) {
        reset();
        runtime.register_vm(*this);
        registered_ = true;
    }
    ScopeExit cleanup([this, &runtime, owns_execution_boundary] {
        if (!owns_execution_boundary) return;
        close_all_open_upvalues();
        runtime.unregister_vm(*this);
        registered_ = false;
        reset();
    });

    const std::size_t boundary_depth = frames_.size();
    const std::size_t operand_base = stack_.size();
    for (const Value argument : arguments) stack_.push_back(argument);

    bool frame_pushed = false;
    const auto prepared = invoke_function(callee, static_cast<std::uint32_t>(arguments.size()), operand_base,
        operand_base, this_value, frame_pushed);
    if (!prepared) {
        stack_.resize(operand_base);
        return prepared.error();
    }
    if (prepared.completion().is_throw()) {
        stack_.resize(operand_base);
        return prepared.completion();
    }
    if (!frame_pushed) {
        const Value value = stack_.back();
        stack_.resize(operand_base);
        return Completion::normal(value);
    }
    return execute_loop(boundary_depth);
}


ExecutionResult VM::construct(Value constructor, std::span<const Value> arguments) {
    const auto constructor_validation = context_->validate(constructor);
    if (!constructor_validation) return constructor_validation.error();
    for (const Value argument : arguments) {
        const auto validation = context_->validate(argument);
        if (!validation) return validation.error();
    }

    const bool owns_execution_boundary = !registered_;
    Runtime& runtime = context_->runtime();
    if (owns_execution_boundary) {
        reset();
        runtime.register_vm(*this);
        registered_ = true;
    }
    ScopeExit cleanup([this, &runtime, owns_execution_boundary] {
        if (!owns_execution_boundary) return;
        close_all_open_upvalues();
        runtime.unregister_vm(*this);
        registered_ = false;
        reset();
    });

    const std::size_t boundary_depth = frames_.size();
    const std::size_t operand_base = stack_.size();
    stack_.push_back(constructor);
    for (const Value argument : arguments) stack_.push_back(argument);

    bool frame_pushed = false;
    const auto prepared = invoke_construct(constructor, static_cast<std::uint32_t>(arguments.size()), operand_base,
        operand_base + 1U, frame_pushed);
    if (!prepared) {
        stack_.resize(operand_base);
        return prepared.error();
    }
    if (prepared.completion().is_throw()) {
        stack_.resize(operand_base);
        return prepared.completion();
    }
    if (!frame_pushed) {
        const Value value = stack_.back();
        stack_.resize(operand_base);
        return Completion::normal(value);
    }
    return execute_loop(boundary_depth);
}


ExecutionResult VM::resume_generator(Value generator, Value input) {
    const auto validation = context_->validate(generator); if (!validation) return validation.error();
    if (!generator.is_object() || !generator.as_heap_object()->is_generator) return Error{ErrorCode::type_error, "Generator.next receiver is not a generator"};
    auto* object = generator.as_heap_object();
    auto& state = object->generator_state;
    if (state.state == detail::GeneratorStateKind::executing) return Error{ErrorCode::type_error, "generator is already executing"};

    Value value = Value::undefined();
    bool done = true;
    if (state.state != detail::GeneratorStateKind::completed) {
        if (!state.code) return Error{ErrorCode::internal, "generator has no function code"};

        if (frames_.empty()) {
            const auto result = run_impl(state.code->chunk, state.module_environment, object, input);
            if (!result) { state.state = detail::GeneratorStateKind::completed; return result.error(); }
            if (result.completion().is_throw()) { state.state = detail::GeneratorStateKind::completed; return result.completion(); }
            value = result.completion().value();
        } else {
            const std::size_t boundary_depth = frames_.size();
            const std::size_t stack_base = stack_.size();
            const bool resuming_yield = state.state == detail::GeneratorStateKind::suspended_yield;
            state.state = detail::GeneratorStateKind::executing;
            stack_.insert(stack_.end(), state.stack.begin(), state.stack.end());
            state.stack.clear();
            frames_.push_back(Frame{&state.code->chunk, state.pc, state.last_instruction_pc, stack_base, state.this_value,
                std::move(state.locals), std::move(state.actual_arguments), state.upvalues, std::move(state.captured_locals), {},
                std::nullopt, ExecutionContext{state.realm, state.function, state.module_environment}});
            if (resuming_yield) stack_.push_back(input);
            if (frames_.size() > maximum_frame_depth_) maximum_frame_depth_ = frames_.size();
            const Value previous_active = active_generator_;
            active_generator_ = generator;
            const auto result = execute_loop(boundary_depth, object);
            active_generator_ = previous_active;
            if (!result) { state.state = detail::GeneratorStateKind::completed; return result.error(); }
            if (result.completion().is_throw()) { state.state = detail::GeneratorStateKind::completed; return result.completion(); }
            value = result.completion().value();
        }
        done = !generator_suspended_;
        generator_suspended_ = false;
        if (done) state.state = detail::GeneratorStateKind::completed;
    }
    Value iterator_result = context_->object_in_realm(*object->realm);
    auto a = context_->set_own_property(iterator_result, "value", value); if (!a) return a.error();
    auto b = context_->set_own_property(iterator_result, "done", Value::boolean(done)); if (!b) return b.error();
    return Completion::normal(iterator_result);
}


ExecutionResult VM::run_module(const bytecode::BytecodeChunk& chunk, detail::HeapModuleEnvironment& environment) {
    return run_impl(chunk, &environment);
}

ExecutionResult VM::run_impl(const bytecode::BytecodeChunk& chunk, detail::HeapModuleEnvironment* module_environment, detail::HeapObject* generator, Value resume_input) {
    bytecode::BytecodeVerifier verifier;
    const auto verification = verifier.verify(chunk);
    if (!verification) return verification.error();

    reset();
    Runtime& runtime = context_->runtime();
    if (generator != nullptr) active_generator_ = Value(generator);
    runtime.register_vm(*this);
    registered_ = true;
    ScopeExit cleanup([this, &runtime] {
        close_all_open_upvalues();
        runtime.unregister_vm(*this);
        registered_ = false;
    });

    root_local_count_ = chunk.local_count();
    if (generator != nullptr) {
        auto& state = generator->generator_state;
        const bool resuming_yield = state.state == detail::GeneratorStateKind::suspended_yield;
        state.state = detail::GeneratorStateKind::executing;
        stack_ = std::move(state.stack);
        frames_.push_back(Frame{&chunk, state.pc, state.last_instruction_pc, 0U, state.this_value, std::move(state.locals), std::move(state.actual_arguments), state.upvalues, std::move(state.captured_locals), {}, std::nullopt, ExecutionContext{state.realm, state.function, state.module_environment}});
        if (resuming_yield) stack_.push_back(resume_input);
    } else {
        std::vector<BindingSlot> root_locals = make_local_slots(chunk);
        std::vector<detail::HeapUpvalue*> root_captures(root_locals.size(), nullptr);
        frames_.push_back(Frame{&chunk, 0U, 0U, 0U, Value::undefined(), std::move(root_locals), {}, {}, std::move(root_captures), {}, std::nullopt, ExecutionContext{&context_->realm(), nullptr, module_environment, &context_->realm().global_environment(), &context_->realm().global_environment(), nullptr}});
    }
    maximum_frame_depth_ = 1U;
    if (module_environment != nullptr && generator == nullptr) {
        if (module_environment->bindings.size() < static_cast<std::size_t>(chunk.module_binding_count())) {
            return Error{ErrorCode::vm_error, "module environment is smaller than bytecode module binding count"};
        }
        Frame& root = frames_.back();
        for (const auto& exported : chunk.module_exports()) {
            if (exported.local_index >= root.locals.size() || exported.module_index >= module_environment->bindings.size()) {
                return Error{ErrorCode::vm_error, "module export binding metadata is out of bounds"};
            }
            module_environment->bindings[exported.module_index].local_upvalue = capture_local(root, exported.local_index);
        }
    }
    stack_.reserve(verification->maximum_stack_depth);

    return execute_loop(0U, generator);
}

ExecutionResult VM::execute_loop(std::size_t boundary_depth, detail::HeapObject* generator) {
    while (!frames_.empty()) {
        Frame& frame = frames_.back();
        const auto& code = frame.chunk->code();
        if (frame.pc >= code.size()) return Error{ErrorCode::vm_error, "execution frame reached end without RETURN"};
        const std::size_t instruction_pc = frame.pc;
        frame.last_instruction_pc = instruction_pc;
        const auto opcode = static_cast<bytecode::OpCode>(code[frame.pc++]);

        switch (opcode) {
        case bytecode::OpCode::constant: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const Value value = frame.chunk->constants()[index];
            const auto value_validation = context_->validate(value); if (!value_validation) return value_validation.error();
            stack_.push_back(value);
            break;
        }
        case bytecode::OpCode::undefined: stack_.push_back(Value::undefined()); break;
        case bytecode::OpCode::get_local: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const auto value = get_binding_slot(frame.locals[index]); if (!value) return value.error();
            stack_.push_back(*value);
            break;
        }
        case bytecode::OpCode::set_local: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const auto set = set_binding_slot(frame.locals[index], stack_.back()); if (!set) return set.error();
            break;
        }
        case bytecode::OpCode::initialize_local: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const auto initialized = initialize_binding_slot(frame.locals[index], stack_.back()); if (!initialized) return initialized.error();
            break;
        }
        case bytecode::OpCode::reset_local: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (frame.captured_locals[index] != nullptr) {
                frame.captured_locals[index]->close();
                frame.captured_locals[index] = nullptr;
            }
            frame.locals[index].value = Value::undefined();
            frame.locals[index].state = BindingState::Uninitialized;
            break;
        }
        case bytecode::OpCode::clone_local_binding: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (frame.captured_locals[index] != nullptr) {
                frame.captured_locals[index]->close();
                frame.captured_locals[index] = nullptr;
            }
            break;
        }
        case bytecode::OpCode::get_upvalue: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (frame.upvalues[index]->state() == BindingState::Uninitialized)
                return Error{ErrorCode::reference_error, "cannot access captured lexical binding before initialization"};
            stack_.push_back(frame.upvalues[index]->get());
            break;
        }
        case bytecode::OpCode::get_module: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (frame.execution_context.module_environment == nullptr) return Error{ErrorCode::vm_error, "GET_MODULE executed without a module environment"};
            const auto value = frame.execution_context.module_environment->get(index); if (!value) return value.error();
            stack_.push_back(*value);
            break;
        }
        case bytecode::OpCode::get_name:
        case bytecode::OpCode::get_name_or_undefined: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const Value name_value = frame.chunk->constants()[index];
            if (!name_value.is_string()) return Error{ErrorCode::vm_error, "named environment lookup requires a string constant"};
            context_->ensure_builtins(*frame.execution_context.realm);
            auto& global = frame.execution_context.realm->global_environment();
            const auto name = name_value.as_string();
            if (!global.has_binding(name)) {
                if (opcode == bytecode::OpCode::get_name_or_undefined) {
                    stack_.push_back(Value::undefined());
                    break;
                }
                {
                    Completion completion = Completion::throw_(context_->reference_error("binding '" + std::string(name) + "' is not defined"));
                    if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
                    break;
                }
            }
            const auto value = global.get_binding_value(name);
            if (!value) return value.error();
            stack_.push_back(*value);
            break;
        }
        case bytecode::OpCode::get_this:
            stack_.push_back(frame.this_value);
            break;
        case bytecode::OpCode::get_argument: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            stack_.push_back(index < frame.actual_arguments.size() ? frame.actual_arguments[index] : Value::undefined());
            break;
        }
        case bytecode::OpCode::rest_arguments: {
            const auto start = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            Value rest = context_->array_in_realm(*frame.execution_context.realm);
            const std::size_t begin = std::min<std::size_t>(start, frame.actual_arguments.size());
            for (std::size_t i = begin; i < frame.actual_arguments.size(); ++i) {
                const auto pushed = context_->array_push(rest, frame.actual_arguments[i]);
                if (!pushed) return pushed.error();
            }
            stack_.push_back(rest);
            break;
        }
        case bytecode::OpCode::set_upvalue: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (frame.upvalues[index]->state() == BindingState::Uninitialized)
                return Error{ErrorCode::reference_error, "cannot assign to captured lexical binding before initialization"};
            if (frame.upvalues[index]->immutable())
                return Error{ErrorCode::type_error, "assignment to immutable captured binding"};
            frame.upvalues[index]->set(stack_.back());
            break;
        }
        case bytecode::OpCode::set_module: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (frame.execution_context.module_environment == nullptr) return Error{ErrorCode::vm_error, "SET_MODULE executed without a module environment"};
            const auto set = frame.execution_context.module_environment->set(index, stack_.back()); if (!set) return set.error();
            break;
        }
        case bytecode::OpCode::set_name:
        case bytecode::OpCode::set_name_strict: {
            const auto index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const Value name_value = frame.chunk->constants()[index];
            if (!name_value.is_string()) return Error{ErrorCode::vm_error, "named environment assignment requires a string constant"};
            context_->ensure_builtins(*frame.execution_context.realm);
            auto& global = frame.execution_context.realm->global_environment();
            const auto name = name_value.as_string();
            if (global.has_binding(name)) {
                const auto set = global.set_mutable_binding(name, stack_.back());
                if (!set) return set.error();
                break;
            }
            if (opcode == bytecode::OpCode::set_name_strict)
                {
                    Completion completion = Completion::throw_(context_->reference_error("binding '" + std::string(name) + "' is not defined"));
                    if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
                    break;
                }
            const auto created = global.create_global_var_binding(std::string(name), stack_.back());
            if (!created) return created.error();
            const auto defined = context_->define_own_property(
                frame.execution_context.realm->global_object(),
                context_->property_key(name),
                PropertyDescriptor::data(stack_.back(), true, true, true));
            if (!defined) return defined.error();
            if (!*defined) return Error{ErrorCode::type_error, "cannot create global property '" + std::string(name) + "'"};
            break;
        }
        case bytecode::OpCode::closure: {
            const auto constant_index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const Value prototype_value = frame.chunk->constants()[constant_index];
            const auto prototype_validation = context_->validate(prototype_value); if (!prototype_validation) return prototype_validation.error();
            if (!prototype_value.is_function() || prototype_value.as_heap_function()->code->is_native()) return Error{ErrorCode::vm_error, "CLOSURE constant is not a bytecode function prototype"};
            const auto* prototype = prototype_value.as_heap_function();
            std::vector<detail::HeapUpvalue*> captures;
            captures.reserve(prototype->code->chunk.upvalues().size());
            for (const auto& descriptor : prototype->code->chunk.upvalues()) {
                if (descriptor.source == bytecode::UpvalueSource::local) {
                    if (descriptor.index >= frame.locals.size()) return Error{ErrorCode::vm_error, "CLOSURE local capture index out of bounds"};
                    captures.push_back(capture_local(frame, descriptor.index));
                } else {
                    if (descriptor.index >= frame.upvalues.size()) return Error{ErrorCode::vm_error, "CLOSURE parent-upvalue index out of bounds"};
                    captures.push_back(frame.upvalues[descriptor.index]);
                }
            }
            Value closure = context_->runtime().make_closure(*prototype, std::move(captures), frame.execution_context.module_environment);
            if (prototype->code->this_mode == ThisMode::Lexical) {
                const_cast<detail::HeapFunction*>(closure.as_heap_function())->lexical_this = frame.this_value;
            }
            stack_.push_back(closure);
            break;
        }
        case bytecode::OpCode::new_object: stack_.push_back(context_->object_in_realm(*frame.execution_context.realm)); break;
        case bytecode::OpCode::new_array: stack_.push_back(context_->array_in_realm(*frame.execution_context.realm)); break;
        case bytecode::OpCode::append_element: { if(stack_.size()<frame.stack_base+2U) return Error{ErrorCode::vm_error,"APPEND_ELEMENT stack underflow"}; Value value=stack_.back(); stack_.pop_back(); Value array=stack_.back(); const auto r=context_->array_push(array,value); if(!r) return r.error(); break; }
        case bytecode::OpCode::append_hole: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "APPEND_HOLE stack underflow"};
            Value array = stack_.back();
            const auto length = context_->get_property(array, "length"); if (!length) return length.error();
            const auto set = context_->set_own_property(array, context_->property_key("length"), Value::number(length->as_number() + 1.0)); if (!set) return set.error();
            break;
        }
        case bytecode::OpCode::append_spread: {
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "APPEND_SPREAD stack underflow"};
            Value source = stack_.back(); stack_.pop_back();
            Value array = stack_.back();
            const auto iterator_result = abstract_operations::get_iterator(*context_, source);
            if (!iterator_result) return iterator_result.error();
            if (iterator_result.completion().is_throw()) { if (auto routed = propagate_completion(iterator_result.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            Value iterator = iterator_result.completion().value();
            const auto next_method = abstract_operations::get_method(*context_, iterator, context_->property_key("next"));
            if (!next_method) return next_method.error();
            if (next_method.completion().is_throw()) { if (auto routed = propagate_completion(next_method.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            IteratorRecord record{iterator, next_method.completion().value()};
            while (true) {
                const auto next = abstract_operations::iterator_next(*context_, record); if (!next) return next.error();
                if (next.completion().is_throw()) { if (auto routed = propagate_completion(next.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const auto complete = abstract_operations::iterator_complete(*context_, next.completion().value()); if (!complete) return complete.error();
                if (complete.completion().is_throw()) { if (auto routed = propagate_completion(complete.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                if (abstract_operations::to_boolean(complete.completion().value())) break;
                const auto item = abstract_operations::iterator_value(*context_, next.completion().value()); if (!item) return item.error();
                if (item.completion().is_throw()) { if (auto routed = propagate_completion(item.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const auto pushed = context_->array_push(array, item.completion().value()); if (!pushed) return pushed.error();
            }
            break;
        }
        case bytecode::OpCode::enumerate_keys: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "ENUMERATE_KEYS stack underflow"};
            Value source = stack_.back();
            Value keys_array = context_->array();
            if (!source.is_null() && !source.is_undefined()) {
                if (!source.is_object_like()) source = context_->box_primitive(source);
                std::vector<PropertyKey> seen;
                Value current = source;
                while (current.is_object_like()) {
                    const auto keys = context_->own_property_keys(current); if (!keys) return keys.error();
                    for (const PropertyKey key : *keys) {
                        if (!key.is_atom()) continue;
                        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
                        seen.push_back(key);
                        const auto descriptor = context_->get_own_property_descriptor(current, key); if (!descriptor) return descriptor.error();
                        if (!*descriptor || !descriptor->value().enumerable.value_or(false)) continue;
                        const auto pushed = context_->array_push(keys_array, context_->string(context_->runtime().atom_text(key.atom_id())));
                        if (!pushed) return pushed.error();
                    }
                    const auto prototype = context_->get_prototype(current); if (!prototype) return prototype.error();
                    current = *prototype;
                }
            }
            stack_.back() = keys_array;
            break;
        }
        case bytecode::OpCode::get_iterator: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "GET_ITERATOR stack underflow"};
            const Value source = stack_.back(); stack_.pop_back();
            const auto iterator_result = abstract_operations::get_iterator(*context_, source);
            if (!iterator_result) return iterator_result.error();
            if (iterator_result.completion().is_throw()) {
                if (auto routed = propagate_completion(iterator_result.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!iterator_result.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "GetIterator produced non-normal completion"};
            const Value iterator = iterator_result.completion().value();
            const auto next_method = abstract_operations::get_method(*context_, iterator, context_->property_key("next"));
            if (!next_method) return next_method.error();
            if (next_method.completion().is_throw()) {
                if (auto routed = propagate_completion(next_method.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!next_method.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "GetMethod(next) produced non-normal completion"};
            if (next_method.completion().value().is_undefined()) {
                const auto failure = abstract_operations::call(*context_, Value::undefined(), iterator);
                if (!failure) return failure.error();
                if (auto routed = propagate_completion(failure.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            stack_.push_back(iterator);
            stack_.push_back(next_method.completion().value());
            break;
        }
        case bytecode::OpCode::iterator_next: {
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "ITERATOR_NEXT stack underflow"};
            const Value next_method = stack_.back(); stack_.pop_back();
            const Value iterator = stack_.back(); stack_.pop_back();
            const auto next = abstract_operations::iterator_next(*context_, IteratorRecord{iterator, next_method});
            if (!next) return next.error();
            if (next.completion().is_throw()) {
                if (auto routed = propagate_completion(next.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!next.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "IteratorNext produced non-normal completion"};
            stack_.push_back(next.completion().value());
            break;
        }
        case bytecode::OpCode::iterator_complete: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "ITERATOR_COMPLETE stack underflow"};
            const auto complete = abstract_operations::iterator_complete(*context_, stack_.back());
            if (!complete) return complete.error();
            if (complete.completion().is_throw()) {
                if (auto routed = propagate_completion(complete.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!complete.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "IteratorComplete produced non-normal completion"};
            stack_.back() = complete.completion().value();
            break;
        }
        case bytecode::OpCode::iterator_value: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "ITERATOR_VALUE stack underflow"};
            const auto value = abstract_operations::iterator_value(*context_, stack_.back());
            if (!value) return value.error();
            if (value.completion().is_throw()) {
                if (auto routed = propagate_completion(value.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!value.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "IteratorValue produced non-normal completion"};
            stack_.back() = value.completion().value();
            break;
        }
        case bytecode::OpCode::to_object: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "TO_OBJECT stack underflow"};
            Value value = stack_.back();
            if (value.is_null() || value.is_undefined()) return Error{ErrorCode::type_error, "cannot destructure null or undefined"};
            if (!value.is_object_like()) stack_.back() = context_->box_primitive(value);
            break;
        }
        case bytecode::OpCode::copy_object_rest: {
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "COPY_OBJECT_REST stack underflow"};
            Value exclusions = stack_.back(); stack_.pop_back();
            Value source = stack_.back(); stack_.pop_back();
            if (source.is_null() || source.is_undefined()) return Error{ErrorCode::type_error, "cannot destructure null or undefined"};
            if (!source.is_object_like()) source = context_->box_primitive(source);
            Value target = context_->object();
            std::vector<PropertyKey> excluded;
            const auto length = context_->get_property(exclusions, "length"); if (!length) return length.error();
            const auto count = static_cast<std::uint32_t>(length->as_number());
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto item = context_->get_property(exclusions, std::to_string(i)); if (!item) return item.error();
                const auto key = context_->property_key(*item); if (!key) return key.error();
                excluded.push_back(*key);
            }
            const auto keys = context_->own_property_keys(source); if (!keys) return keys.error();
            for (const auto key : *keys) {
                if (std::find(excluded.begin(), excluded.end(), key) != excluded.end()) continue;
                const auto descriptor = context_->get_own_property_descriptor(source, key); if (!descriptor) return descriptor.error();
                if (!*descriptor || !descriptor->value().enumerable.value_or(false)) continue;
                const auto value = context_->get_property_semantic(source, key, source); if (!value) return value.error();
                if (value.completion().is_throw()) { if (auto routed = propagate_completion(value.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                if (!value.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "object rest get produced non-normal completion"};
                const auto set = context_->set_own_property(target, key, value.completion().value()); if (!set) return set.error();
            }
            stack_.push_back(target);
            break;
        }
        case bytecode::OpCode::copy_data_properties: {
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "COPY_DATA_PROPERTIES stack underflow"};
            Value source = stack_.back(); stack_.pop_back();
            Value target = stack_.back();
            if (source.is_null() || source.is_undefined()) break;
            if (!source.is_object_like()) source = context_->box_primitive(source);
            const auto keys = context_->own_property_keys(source); if (!keys) return keys.error();
            for (const auto key : *keys) {
                const auto descriptor = context_->get_own_property_descriptor(source, key); if (!descriptor) return descriptor.error();
                if (!*descriptor || !descriptor->value().enumerable.value_or(false)) continue;
                const auto value = context_->get_property_semantic(source, key, source); if (!value) return value.error();
                if (value.completion().is_throw()) { if (auto routed = propagate_completion(value.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const auto created = abstract_operations::create_data_property(*context_, target, key, value.completion().value()); if (!created) return created.error();
            }
            break;
        }
        case bytecode::OpCode::define_element: {
            if (stack_.size() < frame.stack_base + 3U) return Error{ErrorCode::vm_error, "DEFINE_ELEMENT stack underflow"};
            Value value = stack_.back(); stack_.pop_back();
            Value key_value = stack_.back(); stack_.pop_back();
            Value object = stack_.back();
            const auto key = abstract_operations::to_property_key(*context_, key_value); if (!key) return key.error();
            if (key.completion().is_throw()) { if (auto routed = propagate_completion(key.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            const auto pk = context_->property_key(key.completion().value()); if (!pk) return pk.error();
            const auto created = abstract_operations::create_data_property(*context_, object, *pk, value); if (!created) return created.error();
            break;
        }
        case bytecode::OpCode::define_property: {
            const auto key_index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "DEFINE_PROPERTY stack underflow"};
            const Value value = stack_.back(); stack_.pop_back();
            const Value object = stack_.back();
            const Value key = frame.chunk->constants()[key_index];
            const auto set = context_->set_own_property(object, context_->property_key(key.as_string()), value); if (!set) return set.error();
            break;
        }
        case bytecode::OpCode::define_getter_element: {
            if (stack_.size() < frame.stack_base + 3U) return Error{ErrorCode::vm_error, "DEFINE_GETTER_ELEMENT stack underflow"};
            const Value getter = stack_.back(); stack_.pop_back();
            const Value key_value = stack_.back(); stack_.pop_back();
            const Value object = stack_.back();
            const auto key = abstract_operations::to_property_key(*context_, key_value); if (!key) return key.error();
            if (key.completion().is_throw()) { if (auto routed = propagate_completion(key.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            const auto pk = context_->property_key(key.completion().value()); if (!pk) return pk.error();
            const auto defined = context_->define_own_property(object, *pk,
                PropertyDescriptor::accessor(getter, context_->undefined(), true, true));
            if (!defined) return defined.error();
            if (!*defined) return Error{ErrorCode::type_error, "cannot define object literal getter"};
            break;
        }
        case bytecode::OpCode::define_getter: {
            const auto key_index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "DEFINE_GETTER stack underflow"};
            const Value getter = stack_.back(); stack_.pop_back();
            const Value object = stack_.back();
            const Value key = frame.chunk->constants()[key_index];
            const auto defined = context_->define_own_property(object, context_->property_key(key.as_string()),
                PropertyDescriptor::accessor(getter, context_->undefined(), true, true));
            if (!defined) return defined.error();
            if (!*defined) return Error{ErrorCode::type_error, "cannot define object literal getter"};
            break;
        }
        case bytecode::OpCode::get_property: {
            const auto key_index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "GET_PROPERTY stack underflow"};
            const Value object = stack_.back(); stack_.pop_back();
            const Value key = frame.chunk->constants()[key_index];
            const auto value = context_->get_property_semantic(object, context_->property_key(key.as_string()));
            if (!value) return value.error();
            if (value.completion().is_throw()) {
                if (auto routed = propagate_completion(value.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!value.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "property get produced non-normal completion"};
            stack_.push_back(value.completion().value());
            break;
        }
        case bytecode::OpCode::set_property:
        case bytecode::OpCode::set_property_strict: {
            const auto key_index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "SET_PROPERTY stack underflow"};
            const Value value = stack_.back(); stack_.pop_back();
            const Value object = stack_.back(); stack_.pop_back();
            const Value key = frame.chunk->constants()[key_index];
            const auto set = context_->set_property_semantic(object, context_->property_key(key.as_string()), value);
            if (!set) return set.error();
            if (set.completion().is_throw()) {
                if (auto routed = propagate_completion(set.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!set.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "property set produced non-normal completion"};
            if (!set.completion().value().is_boolean()) return EngineFailure{EngineFailureCode::InternalInvariant, "property set did not return a boolean"};
            if (!set.completion().value().as_boolean() && opcode == bytecode::OpCode::set_property_strict) {
                Completion completion = Completion::throw_(context_->type_error("property is not writable"));
                if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
                break;
            }
            stack_.push_back(value);
            break;
        }
        case bytecode::OpCode::get_element: {
            if(stack_.size()<frame.stack_base+2U) return Error{ErrorCode::vm_error,"GET_ELEMENT stack underflow"};
            Value key=stack_.back(); stack_.pop_back(); Value object=stack_.back(); stack_.pop_back();
            auto r=context_->get_element_semantic(object,key); if(!r) return r.error();
            if (r.completion().is_throw()) { if (auto routed = propagate_completion(r.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            if (!r.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "element get produced non-normal completion"};
            stack_.push_back(r.completion().value()); break;
        }
        case bytecode::OpCode::set_element:
        case bytecode::OpCode::set_element_strict: {
            if(stack_.size()<frame.stack_base+3U) return Error{ErrorCode::vm_error,"SET_ELEMENT stack underflow"};
            Value value=stack_.back(); stack_.pop_back(); Value key=stack_.back(); stack_.pop_back(); Value object=stack_.back(); stack_.pop_back();
            auto r=context_->set_element_semantic(object,key,value); if(!r) return r.error();
            if (r.completion().is_throw()) { if (auto routed = propagate_completion(r.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            if (!r.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "element set produced non-normal completion"};
            if (!r.completion().value().is_boolean()) return EngineFailure{EngineFailureCode::InternalInvariant, "element set did not return a boolean"};
            if (!r.completion().value().as_boolean() && opcode == bytecode::OpCode::set_element_strict) {
                Completion completion = Completion::throw_(context_->type_error("property is not writable"));
                if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
                break;
            }
            stack_.push_back(value); break;
        }
        case bytecode::OpCode::pop: stack_.pop_back(); break;
        case bytecode::OpCode::delete_property:
        case bytecode::OpCode::delete_property_strict: {
            const auto key_index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "DELETE_PROPERTY stack underflow"};
            const Value object = stack_.back(); stack_.pop_back();
            const Value key = frame.chunk->constants()[key_index];
            const auto deleted = context_->delete_property_semantic(object, context_->property_key(key.as_string()));
            if (!deleted) return deleted.error();
            if (deleted.completion().is_throw()) {
                if (auto routed = propagate_completion(deleted.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!deleted.completion().is_normal() || !deleted.completion().value().is_boolean())
                return EngineFailure{EngineFailureCode::InternalInvariant, "property delete did not return a boolean"};
            if (!deleted.completion().value().as_boolean() && opcode == bytecode::OpCode::delete_property_strict) {
                Completion completion = Completion::throw_(context_->type_error("cannot delete non-configurable property in strict code"));
                if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
                break;
            }
            stack_.push_back(deleted.completion().value());
            break;
        }
        case bytecode::OpCode::delete_element:
        case bytecode::OpCode::delete_element_strict: {
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "DELETE_ELEMENT stack underflow"};
            const Value key_value = stack_.back(); stack_.pop_back();
            const Value object = stack_.back(); stack_.pop_back();
            const auto key = abstract_operations::to_property_key(*context_, key_value);
            if (!key) return key.error();
            if (key.completion().is_throw()) { if (auto routed = propagate_completion(key.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            const auto property_key = context_->property_key(key.completion().value());
            if (!property_key) return property_key.error();
            const auto deleted = context_->delete_property_semantic(object, *property_key);
            if (!deleted) return deleted.error();
            if (deleted.completion().is_throw()) {
                if (auto routed = propagate_completion(deleted.completion(), instruction_pc, boundary_depth)) return *routed;
                break;
            }
            if (!deleted.completion().is_normal() || !deleted.completion().value().is_boolean())
                return EngineFailure{EngineFailureCode::InternalInvariant, "element delete did not return a boolean"};
            if (!deleted.completion().value().as_boolean() && opcode == bytecode::OpCode::delete_element_strict) {
                Completion completion = Completion::throw_(context_->type_error("cannot delete non-configurable property in strict code"));
                if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
                break;
            }
            stack_.push_back(deleted.completion().value());
            break;
        }
        case bytecode::OpCode::is_nullish: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "IS_NULLISH stack underflow"};
            const Value value = stack_.back(); stack_.pop_back();
            stack_.push_back(Value::boolean(value.is_null() || value.is_undefined()));
            break;
        }
        case bytecode::OpCode::typeof_: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "TYPEOF stack underflow"};
            const Value value = stack_.back(); stack_.pop_back();
            std::string_view type;
            switch (value.tag()) {
            case ValueTag::undefined: type = "undefined"; break;
            case ValueTag::null: type = "object"; break;
            case ValueTag::boolean: type = "boolean"; break;
            case ValueTag::number: type = "number"; break;
            case ValueTag::string: type = "string"; break;
            case ValueTag::symbol: type = "symbol"; break;
            case ValueTag::function: type = "function"; break;
            case ValueTag::object: type = "object"; break;
            }
            stack_.push_back(context_->string(type));
            break;
        }
        case bytecode::OpCode::negate:
        case bytecode::OpCode::positive:
        case bytecode::OpCode::logical_not:
        case bytecode::OpCode::bitwise_not: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, std::string(bytecode::opcode_name(opcode)) + ": operand stack underflow in current frame"};
            const Value operand = stack_.back();
            stack_.pop_back();
            if (opcode == bytecode::OpCode::logical_not) {
                stack_.push_back(Value::boolean(!abstract_operations::to_boolean(operand)));
                break;
            }
            if (opcode == bytecode::OpCode::bitwise_not) {
                const auto bits = abstract_operations::to_uint32(*context_, operand); if (!bits) return bits.error();
                if (bits.completion().is_throw()) { if (auto routed = propagate_completion(bits.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const auto raw = static_cast<std::uint32_t>(bits.completion().value().as_number());
                const auto signed_result = std::bit_cast<std::int32_t>(~raw);
                stack_.push_back(Value::number(static_cast<double>(signed_result)));
                break;
            }
            const auto number = abstract_operations::to_number(*context_, operand); if (!number) return number.error();
            if (number.completion().is_throw()) { if (auto routed = propagate_completion(number.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            const double numeric = number.completion().value().as_number();
            stack_.push_back(Value::number(opcode == bytecode::OpCode::negate ? -numeric : numeric));
            break;
        }
        case bytecode::OpCode::add:
        case bytecode::OpCode::subtract:
        case bytecode::OpCode::multiply:
        case bytecode::OpCode::divide:
        case bytecode::OpCode::remainder:
        case bytecode::OpCode::exponentiate:
        case bytecode::OpCode::less:
        case bytecode::OpCode::less_equal:
        case bytecode::OpCode::greater:
        case bytecode::OpCode::greater_equal:
        case bytecode::OpCode::equal:
        case bytecode::OpCode::not_equal:
        case bytecode::OpCode::strict_equal:
        case bytecode::OpCode::strict_not_equal:
        case bytecode::OpCode::bitwise_and:
        case bytecode::OpCode::bitwise_or:
        case bytecode::OpCode::bitwise_xor:
        case bytecode::OpCode::shift_left:
        case bytecode::OpCode::shift_right:
        case bytecode::OpCode::shift_right_unsigned:
        case bytecode::OpCode::in_operator:
        case bytecode::OpCode::instanceof_operator: {
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, std::string(bytecode::opcode_name(opcode)) + ": operand stack underflow in current frame"};
            const Value right = stack_.back(); stack_.pop_back();
            const Value left = stack_.back(); stack_.pop_back();

            if (opcode == bytecode::OpCode::in_operator) {
                if (!right.is_object_like()) {
                    Completion completion = Completion::throw_(
                        context_->type_error("right-hand side of 'in' is not an object"));
                    if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
                    break;
                }
                const auto key_value = abstract_operations::to_property_key(*context_, left);
                if (!key_value) return key_value.error();
                if (key_value.completion().is_throw()) { if (auto routed = propagate_completion(key_value.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const auto key = context_->property_key(key_value.completion().value());
                if (!key) return key.error();
                const auto has = abstract_operations::has_property(*context_, right, *key);
                if (!has) return has.error();
                stack_.push_back(Value::boolean(*has));
                break;
            }

            if (opcode == bytecode::OpCode::instanceof_operator) {
                const auto result = abstract_operations::instanceof_operator(*context_, left, right);
                if (!result) return result.error();
                if (result.completion().is_throw()) { if (auto routed = propagate_completion(result.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                if (!result.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "instanceof produced non-normal completion"};
                stack_.push_back(result.completion().value());
                break;
            }

            if (opcode == bytecode::OpCode::add) {
                const auto lhs_primitive = abstract_operations::to_primitive(*context_, left); if (!lhs_primitive) return lhs_primitive.error();
                if (lhs_primitive.completion().is_throw()) { if (auto routed = propagate_completion(lhs_primitive.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const auto rhs_primitive = abstract_operations::to_primitive(*context_, right); if (!rhs_primitive) return rhs_primitive.error();
                if (rhs_primitive.completion().is_throw()) { if (auto routed = propagate_completion(rhs_primitive.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const Value lhs_value = lhs_primitive.completion().value();
                const Value rhs_value = rhs_primitive.completion().value();
                if (lhs_value.is_string() || rhs_value.is_string()) {
                    const auto lhs_string = abstract_operations::to_string(*context_, lhs_value); if (!lhs_string) return lhs_string.error();
                    if (lhs_string.completion().is_throw()) { if (auto routed = propagate_completion(lhs_string.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                    const auto rhs_string = abstract_operations::to_string(*context_, rhs_value); if (!rhs_string) return rhs_string.error();
                    if (rhs_string.completion().is_throw()) { if (auto routed = propagate_completion(rhs_string.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                    stack_.push_back(context_->string(std::string(lhs_string.completion().value().as_string()) + std::string(rhs_string.completion().value().as_string())));
                } else {
                    const auto lhs = abstract_operations::to_numeric(*context_, lhs_value); if (!lhs) return lhs.error();
                    if (lhs.completion().is_throw()) { if (auto routed = propagate_completion(lhs.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                    const auto rhs = abstract_operations::to_numeric(*context_, rhs_value); if (!rhs) return rhs.error();
                    if (rhs.completion().is_throw()) { if (auto routed = propagate_completion(rhs.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                    stack_.push_back(Value::number(lhs.completion().value().as_number() + rhs.completion().value().as_number()));
                }
                break;
            }

            if (opcode == bytecode::OpCode::equal || opcode == bytecode::OpCode::not_equal ||
                opcode == bytecode::OpCode::strict_equal || opcode == bytecode::OpCode::strict_not_equal) {
                const bool strict = opcode == bytecode::OpCode::strict_equal || opcode == bytecode::OpCode::strict_not_equal;
                bool equal = false;
                if (strict) {
                    const auto comparison = abstract_operations::strict_equal(*context_, left, right);
                    if (!comparison) return comparison.error();
                    equal = *comparison;
                } else {
                    const auto comparison = abstract_operations::abstract_equal(*context_, left, right);
                    if (!comparison) return comparison.error();
                    if (comparison.completion().is_throw()) { if (auto routed = propagate_completion(comparison.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                    equal = comparison.completion().value().as_boolean();
                }
                const bool negate = opcode == bytecode::OpCode::not_equal || opcode == bytecode::OpCode::strict_not_equal;
                stack_.push_back(Value::boolean(negate ? !equal : equal));
                break;
            }

            if (opcode == bytecode::OpCode::less || opcode == bytecode::OpCode::less_equal ||
                opcode == bytecode::OpCode::greater || opcode == bytecode::OpCode::greater_equal) {
                const bool reverse = opcode == bytecode::OpCode::greater || opcode == bytecode::OpCode::less_equal;
                const auto relation = reverse
                    ? abstract_operations::abstract_relational_compare(*context_, right, left)
                    : abstract_operations::abstract_relational_compare(*context_, left, right);
                if (!relation) return relation.error();
                if (relation.completion().is_throw()) { if (auto routed = propagate_completion(relation.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const Value relation_value = relation.completion().value();
                bool result = false;
                if (!relation_value.is_undefined()) {
                    result = relation_value.as_boolean();
                    if (opcode == bytecode::OpCode::less_equal || opcode == bytecode::OpCode::greater_equal) result = !result;
                }
                stack_.push_back(Value::boolean(result));
                break;
            }

            if (opcode == bytecode::OpCode::bitwise_and || opcode == bytecode::OpCode::bitwise_or ||
                opcode == bytecode::OpCode::bitwise_xor || opcode == bytecode::OpCode::shift_left ||
                opcode == bytecode::OpCode::shift_right || opcode == bytecode::OpCode::shift_right_unsigned) {
                const auto lhs_bits_result = abstract_operations::to_uint32(*context_, left); if (!lhs_bits_result) return lhs_bits_result.error();
                if (lhs_bits_result.completion().is_throw()) { if (auto routed = propagate_completion(lhs_bits_result.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const auto rhs_bits_result = abstract_operations::to_uint32(*context_, right); if (!rhs_bits_result) return rhs_bits_result.error();
                if (rhs_bits_result.completion().is_throw()) { if (auto routed = propagate_completion(rhs_bits_result.completion(), instruction_pc, boundary_depth)) return *routed; break; }
                const auto lhs_bits = static_cast<std::uint32_t>(lhs_bits_result.completion().value().as_number());
                const auto rhs_bits = static_cast<std::uint32_t>(rhs_bits_result.completion().value().as_number());
                std::uint32_t result_bits = 0;
                bool unsigned_result = false;
                switch (opcode) {
                case bytecode::OpCode::bitwise_and: result_bits = lhs_bits & rhs_bits; break;
                case bytecode::OpCode::bitwise_or: result_bits = lhs_bits | rhs_bits; break;
                case bytecode::OpCode::bitwise_xor: result_bits = lhs_bits ^ rhs_bits; break;
                case bytecode::OpCode::shift_left: result_bits = lhs_bits << (rhs_bits & 31U); break;
                case bytecode::OpCode::shift_right: {
                    const auto shift = rhs_bits & 31U;
                    result_bits = lhs_bits >> shift;
                    if (shift != 0U && (lhs_bits & 0x80000000U) != 0U)
                        result_bits |= 0xffffffffU << (32U - shift);
                    break;
                }
                case bytecode::OpCode::shift_right_unsigned:
                    result_bits = lhs_bits >> (rhs_bits & 31U);
                    unsigned_result = true;
                    break;
                default: return Error{ErrorCode::internal, "unexpected bitwise opcode"};
                }
                if (unsigned_result) {
                    stack_.push_back(Value::number(static_cast<double>(result_bits)));
                } else {
                    stack_.push_back(Value::number(static_cast<double>(std::bit_cast<std::int32_t>(result_bits))));
                }
                break;
            }

            const auto lhs_result = abstract_operations::to_numeric(*context_, left); if (!lhs_result) return lhs_result.error();
            if (lhs_result.completion().is_throw()) { if (auto routed = propagate_completion(lhs_result.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            const auto rhs_result = abstract_operations::to_numeric(*context_, right); if (!rhs_result) return rhs_result.error();
            if (rhs_result.completion().is_throw()) { if (auto routed = propagate_completion(rhs_result.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            const double lhs = lhs_result.completion().value().as_number();
            const double rhs = rhs_result.completion().value().as_number();
            double result = 0.0;
            switch (opcode) {
            case bytecode::OpCode::subtract: result = lhs - rhs; break;
            case bytecode::OpCode::multiply: result = lhs * rhs; break;
            case bytecode::OpCode::divide: result = lhs / rhs; break;
            case bytecode::OpCode::remainder: result = std::fmod(lhs, rhs); break;
            case bytecode::OpCode::exponentiate: result = abstract_operations::number_exponentiate(lhs, rhs); break;
            default: return Error{ErrorCode::internal, "unexpected numeric binary opcode"};
            }
            stack_.push_back(Value::number(result));
            break;
        }
        case bytecode::OpCode::jump_if_false: {
            const auto target = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "JUMP_IF_FALSE stack underflow"};
            const Value condition = stack_.back(); stack_.pop_back();
            if (!abstract_operations::to_boolean(condition)) frame.pc = static_cast<std::size_t>(target);
            break;
        }
        case bytecode::OpCode::jump: {
            const auto target = read_u32(code, frame.pc);
            frame.pc = static_cast<std::size_t>(target);
            break;
        }
        case bytecode::OpCode::break_:
        case bytecode::OpCode::continue_: {
            const auto target = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const Completion completion = opcode == bytecode::OpCode::break_
                ? Completion::break_(target)
                : Completion::continue_(target);
            if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
            break;
        }
        case bytecode::OpCode::call: {
            const auto argument_count = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const std::size_t argc = static_cast<std::size_t>(argument_count);
            if (stack_.size() < frame.stack_base + argc + 1U) return Error{ErrorCode::vm_error, "CALL stack underflow"};
            const std::size_t callee_index = stack_.size() - argc - 1U;
            const Value callee = stack_[callee_index];
            bool frame_pushed = false;
            const auto call = invoke_function(callee, argument_count, callee_index, callee_index + 1U, Value::undefined(), frame_pushed);
            if (!call) return call.error();
            if (call.completion().is_throw()) {
                if (auto routed = propagate_completion(call.completion(), instruction_pc, boundary_depth)) return *routed;
            }
            break;
        }
        case bytecode::OpCode::construct: {
            const auto argument_count = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const std::size_t argc = static_cast<std::size_t>(argument_count);
            if (stack_.size() < frame.stack_base + argc + 1U) return Error{ErrorCode::vm_error, "CONSTRUCT stack underflow"};
            const std::size_t callee_index = stack_.size() - argc - 1U;
            const Value callee = stack_[callee_index];
            bool frame_pushed = false;
            const auto construct_call = invoke_construct(callee, argument_count, callee_index, callee_index + 1U, frame_pushed);
            if (!construct_call) return construct_call.error();
            if (construct_call.completion().is_throw()) {
                if (auto routed = propagate_completion(construct_call.completion(), instruction_pc, boundary_depth)) return *routed;
            }
            break;
        }
        case bytecode::OpCode::call_method: {
            const auto key_index = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const auto argument_count = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const std::size_t argc = static_cast<std::size_t>(argument_count);
            if (stack_.size() < frame.stack_base + argc + 1U) return Error{ErrorCode::vm_error, "CALL_METHOD stack underflow"};
            const std::size_t receiver_index = stack_.size() - argc - 1U;
            const Value receiver = stack_[receiver_index];
            const Value key = frame.chunk->constants()[key_index];
            const auto callee = context_->get_property_semantic(receiver, context_->property_key(key.as_string()));
            if (!callee) return callee.error();
            if (callee.completion().is_throw()) { if (auto routed = propagate_completion(callee.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            if (!callee.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "method lookup produced non-normal completion"};
            bool frame_pushed = false;
            const auto call = invoke_function(callee.completion().value(), argument_count, receiver_index, receiver_index + 1U, receiver, frame_pushed);
            if (!call) return call.error();
            if (call.completion().is_throw()) {
                if (auto routed = propagate_completion(call.completion(), instruction_pc, boundary_depth)) return *routed;
            }
            break;
        }
        case bytecode::OpCode::call_element: {
            const auto argument_count = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const std::size_t argc = static_cast<std::size_t>(argument_count);
            if (stack_.size() < frame.stack_base + argc + 2U) return Error{ErrorCode::vm_error, "CALL_ELEMENT stack underflow"};
            const std::size_t receiver_index = stack_.size() - argc - 2U;
            const std::size_t key_index = receiver_index + 1U;
            const Value receiver = stack_[receiver_index];
            const Value key = stack_[key_index];
            const auto callee = context_->get_element_semantic(receiver, key); if (!callee) return callee.error();
            if (callee.completion().is_throw()) { if (auto routed = propagate_completion(callee.completion(), instruction_pc, boundary_depth)) return *routed; break; }
            if (!callee.completion().is_normal()) return EngineFailure{EngineFailureCode::InternalInvariant, "computed method lookup produced non-normal completion"};
            bool frame_pushed = false;
            const auto call = invoke_function(callee.completion().value(), argument_count, receiver_index, key_index + 1U, receiver, frame_pushed);
            if (!call) return call.error();
            if (call.completion().is_throw()) {
                if (auto routed = propagate_completion(call.completion(), instruction_pc, boundary_depth)) return *routed;
            }
            break;
        }
        case bytecode::OpCode::call_with_this: {
            const auto argument_count = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            const std::size_t argc = static_cast<std::size_t>(argument_count);
            if (stack_.size() < frame.stack_base + argc + 2U) return Error{ErrorCode::vm_error, "CALL_WITH_THIS stack underflow"};
            const std::size_t callee_index = stack_.size() - argc - 2U;
            const Value callee = stack_[callee_index];
            const Value this_value = stack_[callee_index + 1U];
            bool frame_pushed = false;
            const auto call = invoke_function(callee, argument_count, callee_index, callee_index + 2U, this_value, frame_pushed);
            if (!call) return call.error();
            if (call.completion().is_throw()) {
                if (auto routed = propagate_completion(call.completion(), instruction_pc, boundary_depth)) return *routed;
            }
            break;
        }
        case bytecode::OpCode::call_spread:
        case bytecode::OpCode::construct_spread: {
            if (stack_.size() < frame.stack_base + 2U) return Error{ErrorCode::vm_error, "spread call stack underflow"};
            Value args_array = stack_.back(); stack_.pop_back();
            const auto length = context_->get_property(args_array, "length"); if (!length) return length.error();
            const auto argc = static_cast<std::uint32_t>(length->as_number());
            for (std::uint32_t i=0; i<argc; ++i) { const auto a=context_->get_property(args_array, std::to_string(i)); if(!a)return a.error(); stack_.push_back(*a); }
            const std::size_t callee_index = stack_.size() - static_cast<std::size_t>(argc) - 1U;
            const Value callee = stack_[callee_index]; bool frame_pushed=false;
            const auto result = opcode == bytecode::OpCode::call_spread
                ? invoke_function(callee, argc, callee_index, callee_index+1U, Value::undefined(), frame_pushed)
                : invoke_construct(callee, argc, callee_index, callee_index+1U, frame_pushed);
            if (!result) return result.error();
            if (result.completion().is_throw()) { if (auto routed=propagate_completion(result.completion(), instruction_pc, boundary_depth)) return *routed; }
            break;
        }
        case bytecode::OpCode::call_method_spread: {
            const auto key_index=read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if(stack_.size()<frame.stack_base+2U) return Error{ErrorCode::vm_error,"CALL_METHOD_SPREAD stack underflow"};
            Value args_array=stack_.back(); stack_.pop_back(); Value receiver=stack_.back();
            const auto length=context_->get_property(args_array,"length"); if(!length)return length.error(); const auto argc=static_cast<std::uint32_t>(length->as_number());
            for(std::uint32_t i=0;i<argc;++i){const auto a=context_->get_property(args_array,std::to_string(i));if(!a)return a.error();stack_.push_back(*a);} 
            const Value key=frame.chunk->constants()[key_index]; const auto callee=context_->get_property_semantic(receiver,context_->property_key(key.as_string())); if(!callee)return callee.error();
            if(callee.completion().is_throw()){if(auto routed=propagate_completion(callee.completion(),instruction_pc,boundary_depth))return *routed;break;}
            const std::size_t receiver_index=stack_.size()-static_cast<std::size_t>(argc)-1U; bool frame_pushed=false;
            const auto result=invoke_function(callee.completion().value(),argc,receiver_index,receiver_index+1U,receiver,frame_pushed); if(!result)return result.error();
            if(result.completion().is_throw()){if(auto routed=propagate_completion(result.completion(),instruction_pc,boundary_depth))return *routed;} break;
        }
        case bytecode::OpCode::call_with_this_spread: {
            if (stack_.size() < frame.stack_base + 3U) return Error{ErrorCode::vm_error, "CALL_WITH_THIS_SPREAD stack underflow"};
            Value args_array = stack_.back(); stack_.pop_back();
            const auto length = context_->get_property(args_array, "length"); if (!length) return length.error();
            const auto argc = static_cast<std::uint32_t>(length->as_number());
            for (std::uint32_t i = 0; i < argc; ++i) { const auto a=context_->get_property(args_array,std::to_string(i)); if(!a)return a.error(); stack_.push_back(*a); }
            const std::size_t callee_index = stack_.size() - static_cast<std::size_t>(argc) - 2U;
            const Value callee = stack_[callee_index];
            const Value this_value = stack_[callee_index + 1U];
            bool frame_pushed = false;
            const auto result = invoke_function(callee, argc, callee_index, callee_index + 2U, this_value, frame_pushed);
            if (!result) return result.error();
            if (result.completion().is_throw()) { if (auto routed=propagate_completion(result.completion(),instruction_pc,boundary_depth)) return *routed; }
            break;
        }
        case bytecode::OpCode::call_element_spread: {
            if(stack_.size()<frame.stack_base+3U) return Error{ErrorCode::vm_error,"CALL_ELEMENT_SPREAD stack underflow"};
            Value args_array=stack_.back(); stack_.pop_back(); Value key=stack_.back(); stack_.pop_back(); Value receiver=stack_.back();
            const auto length=context_->get_property(args_array,"length"); if(!length)return length.error(); const auto argc=static_cast<std::uint32_t>(length->as_number());
            for(std::uint32_t i=0;i<argc;++i){const auto a=context_->get_property(args_array,std::to_string(i));if(!a)return a.error();stack_.push_back(*a);} 
            const auto callee=context_->get_element_semantic(receiver,key); if(!callee)return callee.error();
            if(callee.completion().is_throw()){if(auto routed=propagate_completion(callee.completion(),instruction_pc,boundary_depth))return *routed;break;}
            const std::size_t receiver_index=stack_.size()-static_cast<std::size_t>(argc)-1U; bool frame_pushed=false;
            const auto result=invoke_function(callee.completion().value(),argc,receiver_index,receiver_index+1U,receiver,frame_pushed); if(!result)return result.error();
            if(result.completion().is_throw()){if(auto routed=propagate_completion(result.completion(),instruction_pc,boundary_depth))return *routed;} break;
        }
        case bytecode::OpCode::throw_: {
            if (stack_.size() <= frame.stack_base) return Error{ErrorCode::vm_error, "THROW requires one value"};
            const Value exception = stack_.back();
            if (auto routed = propagate_completion(Completion::throw_(exception), instruction_pc, boundary_depth)) return *routed;
            break;
        }
        case bytecode::OpCode::end_finally: {
            const auto finally_start = read_u32(code, frame.pc); frame.pc += sizeof(std::uint32_t);
            if (!frame.pending_completions.empty() && frame.pending_completions.back().finally_start == finally_start) {
                const Completion completion = frame.pending_completions.back().completion;
                frame.pending_completions.pop_back();
                if (auto routed = propagate_completion(completion, instruction_pc, boundary_depth)) return *routed;
            }
            break;
        }
        case bytecode::OpCode::yield_: {
            if (generator == nullptr) return Error{ErrorCode::vm_error, "YIELD executed outside generator resumption"};
            if (stack_.size() != frame.stack_base + 1U) return Error{ErrorCode::vm_error, "YIELD requires exactly one current-frame yielded value"};
            const Value yielded = stack_.back();
            stack_.pop_back();
            auto& state = generator->generator_state;
            state.pc = frame.pc;
            state.last_instruction_pc = frame.last_instruction_pc;
            state.this_value = frame.this_value;
            state.locals = std::move(frame.locals);
            state.actual_arguments = std::move(frame.actual_arguments);
            state.stack.assign(stack_.begin() + static_cast<std::ptrdiff_t>(frame.stack_base), stack_.end());
            stack_.resize(frame.stack_base);
            state.upvalues = frame.upvalues;
            state.captured_locals = std::move(frame.captured_locals);
            state.module_environment = frame.execution_context.module_environment;
            state.state = detail::GeneratorStateKind::suspended_yield;
            frames_.pop_back();
            generator_suspended_ = true;
            return Completion::normal(yielded);
        }
        case bytecode::OpCode::return_: {
            if (stack_.size() != frame.stack_base + 1U) return Error{ErrorCode::vm_error, "RETURN requires exactly one current-frame result value"};
            const Value result = stack_.back();
            if (auto routed = propagate_completion(Completion::return_(result), instruction_pc, boundary_depth)) {
                if (generator != nullptr && routed->completion().is_normal()) generator->generator_state.state = detail::GeneratorStateKind::completed;
                return *routed;
            }
            break;
        }
        }
    }
    return Error{ErrorCode::vm_error, "execution ended without RETURN"};
}

} // namespace js
