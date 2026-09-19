#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <js/bytecode/chunk.hpp>
#include <js/bytecode/opcode.hpp>
#include <js/result.hpp>
#include <js/value.hpp>

namespace js::bytecode {

class BytecodeBuilder final {
public:
    [[nodiscard]] Result<std::uint32_t> add_constant(Value value);

    void emit(OpCode opcode);
    void emit_constant(std::uint32_t constant_index);
    void emit_local(OpCode opcode, std::uint32_t local_index);
    void emit_upvalue(OpCode opcode, std::uint32_t upvalue_index);
    void emit_module(OpCode opcode, std::uint32_t module_index);
    void emit_name(OpCode opcode, std::uint32_t name_constant_index);
    // Resolve a binding once; the returned slot is internal to the VM frame.
    void emit_dynamic_reference(std::uint32_t name_constant_index, std::uint32_t fallback_index, bool strict);
    void emit_argument(std::uint32_t index);
    void emit_rest_arguments(std::uint32_t start_index);
    void emit_closure(std::uint32_t function_constant_index);
    void emit_property(OpCode opcode, std::uint32_t key_constant_index);
    void emit_call(std::uint32_t argument_count);
    void emit_call_with_this(std::uint32_t argument_count);
    void emit_call_with_this_spread();
    void emit_construct(std::uint32_t argument_count);
    void emit_method_call(std::uint32_t key_constant_index, std::uint32_t argument_count);
    void emit_element_call(std::uint32_t argument_count);
    void emit_method_call_spread(std::uint32_t key_constant_index);
    void emit_end_finally(std::uint32_t finally_start);
    void add_exception_handler(ExceptionHandler handler) { chunk_.exception_handlers_.push_back(handler); }
    [[nodiscard]] std::size_t emit_jump(OpCode opcode);
    void patch_jump(std::size_t operand_offset, std::uint32_t target);
    [[nodiscard]] std::size_t offset() const noexcept { return chunk_.code_.size(); }
    void set_local_count(std::uint32_t local_count) noexcept { chunk_.local_count_ = local_count; }
    void set_local_binding_states(std::vector<BindingState> states) { chunk_.local_binding_states_ = std::move(states); }
    void set_local_binding_immutable(std::vector<bool> immutable) { chunk_.local_binding_immutable_ = std::move(immutable); }
    void set_upvalues(std::vector<UpvalueDescriptor> upvalues) { chunk_.upvalues_ = std::move(upvalues); }
    void set_module_binding_count(std::uint32_t count) noexcept { chunk_.module_binding_count_ = count; }
    void set_module_exports(std::vector<ModuleExportBinding> exports) { chunk_.module_exports_ = std::move(exports); }

    [[nodiscard]] const BytecodeChunk& chunk() const noexcept { return chunk_; }
    [[nodiscard]] BytecodeChunk finish() && noexcept { return std::move(chunk_); }

private:
    void emit_u32(std::uint32_t value);

    BytecodeChunk chunk_;
};

} // namespace js::bytecode
