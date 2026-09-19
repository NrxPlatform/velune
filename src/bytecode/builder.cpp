#include <js/bytecode/builder.hpp>

#include <limits>

#include <js/error.hpp>

namespace js::bytecode {

Result<std::uint32_t> BytecodeBuilder::add_constant(Value value) {
    if (chunk_.constants_.size() >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return Error{ErrorCode::bytecode_error, "constant pool exceeds 32-bit index space"};
    }
    const auto index = static_cast<std::uint32_t>(chunk_.constants_.size());
    chunk_.constants_.push_back(value);
    return index;
}

void BytecodeBuilder::emit(OpCode opcode) { 
    chunk_.code_.push_back(static_cast<std::uint8_t>(opcode));
}

void BytecodeBuilder::emit_u32(std::uint32_t value) {
    chunk_.code_.push_back(static_cast<std::uint8_t>(value & 0xffU));
    chunk_.code_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    chunk_.code_.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    chunk_.code_.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void BytecodeBuilder::emit_constant(std::uint32_t constant_index) {
    emit(OpCode::constant);
    emit_u32(constant_index);
}

void BytecodeBuilder::emit_local(OpCode opcode, std::uint32_t local_index) {
    emit(opcode);
    emit_u32(local_index);
}

void BytecodeBuilder::emit_upvalue(OpCode opcode, std::uint32_t upvalue_index) {
    emit(opcode);
    emit_u32(upvalue_index);
}

void BytecodeBuilder::emit_module(OpCode opcode, std::uint32_t module_index) {
    emit(opcode);
    emit_u32(module_index);
}

void BytecodeBuilder::emit_name(OpCode opcode, std::uint32_t name_constant_index) {
    emit(opcode);
    emit_u32(name_constant_index);
}

void BytecodeBuilder::emit_dynamic_reference(std::uint32_t name_constant_index, std::uint32_t fallback_index, bool strict, std::uint32_t slot) {
    emit(OpCode::resolve_dynamic_ref);
    emit_u32(name_constant_index);
    emit_u32((fallback_index & 0x7fffffffU) | (strict ? 0x80000000U : 0U));
    emit_u32(slot);
}

void BytecodeBuilder::emit_argument(std::uint32_t index) {
    emit(OpCode::get_argument);
    emit_u32(index);
}

void BytecodeBuilder::emit_rest_arguments(std::uint32_t start_index) {
    emit(OpCode::rest_arguments);
    emit_u32(start_index);
}

void BytecodeBuilder::emit_closure(std::uint32_t function_constant_index) {
    emit(OpCode::closure);
    emit_u32(function_constant_index);
}

void BytecodeBuilder::emit_property(OpCode opcode, std::uint32_t key_constant_index) {
    emit(opcode);
    emit_u32(key_constant_index);
}

void BytecodeBuilder::emit_call(std::uint32_t argument_count) {
    emit(OpCode::call);
    emit_u32(argument_count);
}

void BytecodeBuilder::emit_call_with_this(std::uint32_t argument_count) {
    emit(OpCode::call_with_this);
    emit_u32(argument_count);
}

void BytecodeBuilder::emit_call_with_this_spread() {
    emit(OpCode::call_with_this_spread);
}

void BytecodeBuilder::emit_construct(std::uint32_t argument_count) {
    emit(OpCode::construct);
    emit_u32(argument_count);
}

void BytecodeBuilder::emit_method_call(std::uint32_t key_constant_index, std::uint32_t argument_count) {
    emit(OpCode::call_method);
    emit_u32(key_constant_index);
    emit_u32(argument_count);
}

void BytecodeBuilder::emit_element_call(std::uint32_t argument_count) {
    emit(OpCode::call_element);
    emit_u32(argument_count);
}


void BytecodeBuilder::emit_method_call_spread(std::uint32_t key_constant_index) {
    emit(OpCode::call_method_spread);
    emit_u32(key_constant_index);
}

void BytecodeBuilder::emit_end_finally(std::uint32_t finally_start) {
    emit(OpCode::end_finally);
    emit_u32(finally_start);
}

std::size_t BytecodeBuilder::emit_jump(OpCode opcode) {
    emit(opcode);
    const std::size_t operand_offset = chunk_.code_.size();
    emit_u32(0xffffffffU);
    return operand_offset;
}

void BytecodeBuilder::patch_jump(std::size_t operand_offset, std::uint32_t target) {
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        chunk_.code_[operand_offset + i] = static_cast<std::uint8_t>((target >> (8U * i)) & 0xffU);
    }
}

} // namespace js::bytecode
