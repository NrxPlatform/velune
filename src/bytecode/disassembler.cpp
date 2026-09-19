#include <js/bytecode/disassembler.hpp>

#include <cstdint>
#include <iomanip>
#include <sstream>

#include <js/bytecode/opcode.hpp>
#include <js/bytecode/verifier.hpp>

namespace js::bytecode {
namespace {
[[nodiscard]] std::uint32_t read_u32(const std::vector<std::uint8_t>& code, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(code[offset]) |
           (static_cast<std::uint32_t>(code[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(code[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(code[offset + 3]) << 24U);
}
[[nodiscard]] std::size_t operand_count(OpCode opcode) noexcept {
    switch (opcode) {
    case OpCode::get_dynamic_ref: case OpCode::typeof_dynamic_ref: case OpCode::this_dynamic_ref: case OpCode::put_dynamic_ref: case OpCode::delete_dynamic_ref: case OpCode::release_dynamic_ref:
    case OpCode::constant: case OpCode::get_local: case OpCode::set_local: case OpCode::initialize_local: case OpCode::reset_local: case OpCode::clone_local_binding: case OpCode::get_upvalue: case OpCode::get_module: case OpCode::get_name: case OpCode::get_name_or_undefined: case OpCode::set_upvalue: case OpCode::set_module: case OpCode::set_name: case OpCode::set_name_strict:
    case OpCode::closure: case OpCode::define_property: case OpCode::define_getter: case OpCode::get_property: case OpCode::set_property: case OpCode::set_property_strict:
    case OpCode::delete_property: case OpCode::delete_property_strict:
    case OpCode::jump_if_false: case OpCode::jump: case OpCode::call: case OpCode::construct: case OpCode::call_element: case OpCode::end_finally: return 1U;
    case OpCode::enter_with: return 1U;
    case OpCode::resolve_dynamic_ref: return 3U;
    case OpCode::call_method: return 2U;
    case OpCode::call_method_spread: return 1U;
    default: return 0U;
    }
}
}

Result<std::string> Disassembler::disassemble(const BytecodeChunk& chunk) const {
    BytecodeVerifier verifier;
    const auto verification = verifier.verify(chunk);
    if (!verification) return verification.error();
    std::ostringstream out;
    const auto& code = chunk.code();
    std::size_t pc = 0;
    while (pc < code.size()) {
        const std::size_t instruction_pc = pc;
        const auto opcode = static_cast<OpCode>(code[pc++]);
        out << std::setw(4) << std::setfill('0') << instruction_pc << "  " << opcode_name(opcode);
        const auto count = operand_count(opcode);
        if (count >= 1U) {
            const auto operand = read_u32(code, pc); pc += sizeof(std::uint32_t);
            out << ' ' << operand;
            if (opcode == OpCode::constant || opcode == OpCode::closure || opcode == OpCode::define_property || opcode == OpCode::define_getter || opcode == OpCode::get_property || opcode == OpCode::set_property || opcode == OpCode::set_property_strict || opcode == OpCode::delete_property || opcode == OpCode::delete_property_strict || opcode == OpCode::call_method || opcode == OpCode::get_name || opcode == OpCode::get_name_or_undefined || opcode == OpCode::set_name || opcode == OpCode::set_name_strict || opcode == OpCode::resolve_dynamic_ref) out << "  ; " << chunk.constants()[operand].to_debug_string();
        }
        if (count >= 2U) { const auto operand2 = read_u32(code, pc); pc += sizeof(std::uint32_t); out << (opcode == OpCode::resolve_dynamic_ref ? " fallback=" : " argc=") << operand2; }
        if (count >= 3U) { const auto slot = read_u32(code, pc); pc += sizeof(std::uint32_t); out << " slot=" << slot; }
        out << '\n';
    }
    if (!chunk.module_exports().empty()) {
        out << "module-exports:\n";
        for (const auto& exported : chunk.module_exports()) {
            out << "  local " << exported.local_index << " -> module " << exported.module_index << '\n';
        }
    }
    if (!chunk.exception_handlers().empty()) {
        out << "handlers:\n";
        for (const auto& handler : chunk.exception_handlers()) {
            out << "  try=[" << handler.try_start << ',' << handler.try_end << ')';
            if (handler.catch_start != no_handler_target) out << " catch=[" << handler.catch_start << ',' << handler.catch_end << ')';
            if (handler.finally_start != no_handler_target) out << " finally=[" << handler.finally_start << ',' << handler.finally_end << ')';
            out << '\n';
        }
    }
    return out.str();
}
} // namespace js::bytecode
