#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <js/bytecode/chunk.hpp>
#include <js/context.hpp>
#include <js/completion.hpp>
#include <js/environment.hpp>
#include <js/dynamic_reference.hpp>
#include <js/execution_context.hpp>
#include <js/result.hpp>
#include <js/value.hpp>

namespace js {

namespace detail { struct HeapUpvalue; struct HeapModuleEnvironment; struct HeapObject; }

class VM final {
    friend class Runtime;
public:
    explicit VM(Context& context) noexcept : context_(&context) {}

    [[nodiscard]] ExecutionResult run(const bytecode::BytecodeChunk& chunk);
    [[nodiscard]] ExecutionResult call(Value callee, std::span<const Value> arguments, Value this_value = Value::undefined());
    // Same-VM semantic invocation path. If this VM is already executing, the existing
    // frame/stack/GC root domain is preserved and execution stops at the invocation boundary.
    [[nodiscard]] ExecutionResult invoke(Value callee, Value this_value, std::span<const Value> arguments);
    [[nodiscard]] ExecutionResult construct(Value constructor, std::span<const Value> arguments);
    [[nodiscard]] ExecutionResult resume_generator(Value generator, Value input = Value::undefined());
    // Internal execution entry used by the module subsystem.
    [[nodiscard]] ExecutionResult run_module(const bytecode::BytecodeChunk& chunk, detail::HeapModuleEnvironment& environment);
    [[nodiscard]] std::size_t stack_size() const noexcept { return stack_.size(); }
    [[nodiscard]] std::size_t local_count() const noexcept { return root_local_count_; }
    [[nodiscard]] std::size_t maximum_frame_depth() const noexcept { return maximum_frame_depth_; }
private:
    struct PendingFinally final {
        Completion completion;
        std::uint32_t finally_start;
    };

    struct Frame final {
        const bytecode::BytecodeChunk* chunk;
        std::size_t pc;
        std::size_t last_instruction_pc;
        std::size_t stack_base;
        Value this_value;
        std::vector<BindingSlot> locals;
        std::vector<Value> actual_arguments;
        std::vector<detail::HeapUpvalue*> upvalues;
        std::vector<detail::HeapUpvalue*> captured_locals;
        std::vector<DynamicBindingReference> retained_references;
        std::vector<PendingFinally> pending_completions;
        std::optional<Value> construct_receiver;
        ExecutionContext execution_context;
    };

    [[nodiscard]] Result<Value> pop_number_operand(const char* operation_name, std::size_t stack_base);
    [[nodiscard]] ExecutionResult invoke_function(Value callee, std::uint32_t argument_count, std::size_t operand_base, std::size_t argument_start, Value this_value, bool& frame_pushed, std::optional<Value> construct_receiver = std::nullopt);
    [[nodiscard]] ExecutionResult invoke_construct(Value callee, std::uint32_t argument_count, std::size_t operand_base, std::size_t argument_start, bool& frame_pushed);
    [[nodiscard]] detail::HeapUpvalue* capture_local(Frame& frame, std::uint32_t local_index);
    void close_frame_upvalues(Frame& frame) noexcept;
    void close_all_open_upvalues() noexcept;
    void discard_overridden_completions(Frame& frame, std::size_t pc) noexcept;
    void release_abandoned_references(Frame& frame, std::size_t begin, std::size_t end) noexcept;
    [[nodiscard]] const bytecode::ExceptionHandler* find_exception_handler(const Frame& frame, std::size_t pc, bool& use_catch) const noexcept;
    [[nodiscard]] const bytecode::ExceptionHandler* find_finally_handler(const Frame& frame, std::size_t pc) const noexcept;
    [[nodiscard]] std::optional<ExecutionResult> propagate_completion(Completion completion, std::size_t origin_pc, std::size_t boundary_depth);
    [[nodiscard]] ExecutionResult execute_loop(std::size_t boundary_depth, detail::HeapObject* generator = nullptr);
    [[nodiscard]] ExecutionResult run_impl(const bytecode::BytecodeChunk& chunk, detail::HeapModuleEnvironment* module_environment, detail::HeapObject* generator = nullptr, Value resume_input = Value::undefined());
    void reset();

    Context* context_;
    std::vector<Value> stack_;
    std::vector<Frame> frames_;
    std::size_t root_local_count_{0};
    std::size_t maximum_frame_depth_{0};
    bool registered_{false};
    bool generator_suspended_{false};
    Value active_generator_{Value::undefined()};
};

} // namespace js
