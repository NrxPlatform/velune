#include <js/bytecode/verifier.hpp>

#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <js/bytecode/opcode.hpp>
#include <js/error.hpp>

namespace js::bytecode {
namespace {

struct Instruction final {
    OpCode opcode;
    std::uint32_t operand{0};
    std::uint32_t operand2{0};
    std::size_t next_pc{0};
};

[[nodiscard]] std::uint32_t read_u32(const std::vector<std::uint8_t>& code, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(code[offset]) |
           (static_cast<std::uint32_t>(code[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(code[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(code[offset + 3]) << 24U);
}

[[nodiscard]] std::size_t operand_count(OpCode opcode) noexcept {
    switch (opcode) {
    case OpCode::get_dynamic_ref:
    case OpCode::put_dynamic_ref:
    case OpCode::delete_dynamic_ref:
    case OpCode::release_dynamic_ref:
    case OpCode::constant:
    case OpCode::get_local:
    case OpCode::get_argument:
    case OpCode::rest_arguments:
    case OpCode::set_local:
    case OpCode::initialize_local:
    case OpCode::reset_local:
    case OpCode::clone_local_binding:
    case OpCode::get_upvalue:
    case OpCode::get_module:
    case OpCode::get_name:
    case OpCode::get_name_or_undefined:
    case OpCode::set_upvalue:
    case OpCode::set_module:
    case OpCode::set_name:
    case OpCode::set_name_strict:
    case OpCode::closure:
    case OpCode::define_property:
    case OpCode::define_getter:
    case OpCode::get_property:
    case OpCode::set_property:
    case OpCode::set_property_strict:
    case OpCode::delete_property:
    case OpCode::delete_property_strict:
    case OpCode::jump_if_false:
    case OpCode::jump:
    case OpCode::break_:
    case OpCode::continue_:
    case OpCode::call:
    case OpCode::construct:
    case OpCode::call_element:
    case OpCode::call_with_this:
    case OpCode::end_finally:
        return 1U;
    case OpCode::resolve_dynamic_ref:
    case OpCode::call_method:
        return 2U;
    case OpCode::call_method_spread:
        return 1U;
    default:
        return 0U;
    }
}

[[nodiscard]] Error verification_error(std::size_t pc, std::string message) {
    return Error{ErrorCode::bytecode_error,
                 "bytecode verification failed at pc " + std::to_string(pc) + ": " + std::move(message)};
}

} // namespace

Result<VerificationInfo> BytecodeVerifier::verify(const BytecodeChunk& chunk) const {
    const auto& code = chunk.code();
    if (code.empty()) return Error{ErrorCode::bytecode_error, "bytecode verification failed: empty chunk"};
    if (!chunk.local_binding_states().empty() && chunk.local_binding_states().size() != static_cast<std::size_t>(chunk.local_count()))
        return Error{ErrorCode::bytecode_error, "bytecode verification failed: local binding-state metadata size mismatch"};
    if (!chunk.local_binding_immutable().empty() && chunk.local_binding_immutable().size() != static_cast<std::size_t>(chunk.local_count()))
        return Error{ErrorCode::bytecode_error, "bytecode verification failed: local binding-mutability metadata size mismatch"};

    std::unordered_map<std::size_t, Instruction> instructions;
    std::unordered_set<std::size_t> boundaries;
    VerificationInfo info;

    std::size_t pc = 0;
    while (pc < code.size()) {
        const std::size_t instruction_pc = pc;
        boundaries.insert(instruction_pc);
        const auto raw = code[pc++];
        if (raw > static_cast<std::uint8_t>(OpCode::release_dynamic_ref)) return verification_error(instruction_pc, "unknown opcode " + std::to_string(raw));
        const auto opcode = static_cast<OpCode>(raw);
        const std::size_t count = operand_count(opcode);
        if (code.size() - pc < count * sizeof(std::uint32_t)) return verification_error(instruction_pc, "truncated " + std::string(opcode_name(opcode)) + " operand");
        std::uint32_t operand = 0;
        std::uint32_t operand2 = 0;
        if (count >= 1U) { operand = read_u32(code, pc); pc += sizeof(std::uint32_t); }
        if (count >= 2U) { operand2 = read_u32(code, pc); pc += sizeof(std::uint32_t); }

        if ((opcode == OpCode::constant || opcode == OpCode::closure || opcode == OpCode::define_property || opcode == OpCode::define_getter || opcode == OpCode::get_property || opcode == OpCode::set_property || opcode == OpCode::set_property_strict || opcode == OpCode::delete_property || opcode == OpCode::delete_property_strict || opcode == OpCode::call_method || opcode == OpCode::call_method_spread || opcode == OpCode::get_name || opcode == OpCode::get_name_or_undefined || opcode == OpCode::set_name || opcode == OpCode::set_name_strict || opcode == OpCode::resolve_dynamic_ref) && operand >= chunk.constant_count()) {
            return verification_error(instruction_pc, "constant index out of bounds");
        }
        if ((opcode == OpCode::define_property || opcode == OpCode::define_getter || opcode == OpCode::get_property || opcode == OpCode::set_property || opcode == OpCode::set_property_strict || opcode == OpCode::delete_property || opcode == OpCode::delete_property_strict || opcode == OpCode::call_method || opcode == OpCode::call_method_spread || opcode == OpCode::get_name || opcode == OpCode::get_name_or_undefined || opcode == OpCode::set_name || opcode == OpCode::set_name_strict || opcode == OpCode::resolve_dynamic_ref) && !chunk.constants()[operand].is_string()) {
            return verification_error(instruction_pc, "property key constant is not a string value");
        }
        if (opcode == OpCode::closure && (!chunk.constants()[operand].is_function())) return verification_error(instruction_pc, "CLOSURE constant is not a function value");
        if ((opcode == OpCode::get_local || opcode == OpCode::set_local || opcode == OpCode::initialize_local || opcode == OpCode::reset_local || opcode == OpCode::clone_local_binding) && operand >= chunk.local_count()) return verification_error(instruction_pc, "local index out of bounds");
        if ((opcode == OpCode::get_upvalue || opcode == OpCode::set_upvalue) && operand >= chunk.upvalue_count()) return verification_error(instruction_pc, "upvalue index out of bounds");
        if ((opcode == OpCode::get_module || opcode == OpCode::set_module) && operand >= chunk.module_binding_count()) return verification_error(instruction_pc, "module binding index out of bounds");

        if (opcode == OpCode::resolve_dynamic_ref && (operand2 & 0x7fffffffU) > chunk.local_count() + chunk.upvalue_count())
            return verification_error(instruction_pc, "dynamic Reference fallback index out of bounds");
        instructions.emplace(instruction_pc, Instruction{opcode, operand, operand2, pc});
        ++info.instruction_count;
    }
    boundaries.insert(code.size());

    for (const auto& [instruction_pc, instruction] : instructions) {
        if (instruction.opcode == OpCode::jump || instruction.opcode == OpCode::jump_if_false || instruction.opcode == OpCode::break_ || instruction.opcode == OpCode::continue_) {
            const auto target = static_cast<std::size_t>(instruction.operand);
            if (target >= code.size() || boundaries.find(target) == boundaries.end()) return verification_error(instruction_pc, "jump target is not an instruction boundary");
        }
    }

    for (const auto& handler : chunk.exception_handlers()) {
        auto valid_boundary = [&](std::uint32_t target, bool allow_end) {
            if (target == no_handler_target) return true;
            const auto value = static_cast<std::size_t>(target);
            if ((!allow_end && value >= code.size()) || (allow_end && value > code.size())) return false;
            return boundaries.find(value) != boundaries.end();
        };
        if (!valid_boundary(handler.try_start, false) || !valid_boundary(handler.try_end, true) || handler.try_start >= handler.try_end) {
            return Error{ErrorCode::bytecode_error, "bytecode verification failed: invalid try handler range"};
        }
        if ((handler.catch_start == no_handler_target) != (handler.catch_end == no_handler_target)) {
            return Error{ErrorCode::bytecode_error, "bytecode verification failed: incomplete catch handler range"};
        }
        if ((handler.finally_start == no_handler_target) != (handler.finally_end == no_handler_target)) {
            return Error{ErrorCode::bytecode_error, "bytecode verification failed: incomplete finally handler range"};
        }
        if (handler.catch_start != no_handler_target && (!valid_boundary(handler.catch_start, false) || !valid_boundary(handler.catch_end, true) || handler.catch_start >= handler.catch_end)) {
            return Error{ErrorCode::bytecode_error, "bytecode verification failed: invalid catch handler range"};
        }
        if (handler.finally_start != no_handler_target && (!valid_boundary(handler.finally_start, false) || !valid_boundary(handler.finally_end, true) || handler.finally_start >= handler.finally_end)) {
            return Error{ErrorCode::bytecode_error, "bytecode verification failed: invalid finally handler range"};
        }
        if (handler.catch_start == no_handler_target && handler.finally_start == no_handler_target) {
            return Error{ErrorCode::bytecode_error, "bytecode verification failed: exception handler has no target"};
        }
    }

    for (const auto& exported : chunk.module_exports()) {
        if (exported.local_index >= chunk.local_count()) return Error{ErrorCode::bytecode_error, "bytecode verification failed: module export local index out of bounds"};
        if (exported.module_index >= chunk.module_binding_count()) return Error{ErrorCode::bytecode_error, "bytecode verification failed: module export binding index out of bounds"};
    }

    std::unordered_set<std::uint32_t> finally_starts;
    for (const auto& handler : chunk.exception_handlers()) {
        if (handler.finally_start != no_handler_target) finally_starts.insert(handler.finally_start);
    }
    for (const auto& [instruction_pc, instruction] : instructions) {
        if (instruction.opcode == OpCode::end_finally && finally_starts.find(instruction.operand) == finally_starts.end()) {
            return verification_error(instruction_pc, "END_FINALLY does not reference a registered finally handler");
        }
    }

    std::unordered_map<std::size_t, std::size_t> depths;
    std::deque<std::pair<std::size_t, std::size_t>> worklist;
    depths.emplace(0U, 0U);
    worklist.emplace_back(0U, 0U);
    bool saw_reachable_terminal = false;

    auto enqueue = [&](std::size_t target, std::size_t depth) -> Result<void> {
        if (target == code.size()) return Error{ErrorCode::bytecode_error, "bytecode verification failed: reachable path falls off end without RETURN"};
        const auto existing = depths.find(target);
        if (existing != depths.end()) {
            if (existing->second != depth) return verification_error(target, "control-flow merge has inconsistent stack depth");
            return {};
        }
        depths.emplace(target, depth);
        worklist.emplace_back(target, depth);
        return {};
    };

    for (const auto& handler : chunk.exception_handlers()) {
        if (handler.catch_start != no_handler_target) { const auto r = enqueue(handler.catch_start, 1U); if (!r) return r.error(); }
        if (handler.finally_start != no_handler_target) { const auto r = enqueue(handler.finally_start, 0U); if (!r) return r.error(); }
    }

    while (!worklist.empty()) {
        const auto [instruction_pc, incoming_depth] = worklist.front();
        worklist.pop_front();
        const auto found = instructions.find(instruction_pc);
        if (found == instructions.end()) return verification_error(instruction_pc, "control flow targets missing instruction");
        const Instruction& instruction = found->second;
        std::size_t depth = incoming_depth;

        auto require = [&](std::size_t count) -> Result<void> {
            if (depth < count) return verification_error(instruction_pc, std::string(opcode_name(instruction.opcode)) + " has insufficient stack operands");
            return {};
        };

        switch (instruction.opcode) {
        case OpCode::get_dynamic_ref:
        case OpCode::delete_dynamic_ref:
        case OpCode::constant:
        case OpCode::undefined:
        case OpCode::get_local:
        case OpCode::get_upvalue:
        case OpCode::get_module:
        case OpCode::get_name:
        case OpCode::get_name_or_undefined:
        case OpCode::get_this:
        case OpCode::get_argument:
        case OpCode::rest_arguments:
        case OpCode::closure:
        case OpCode::new_object:
        case OpCode::new_array:
            ++depth;
            break;
        case OpCode::append_element:
        case OpCode::append_spread: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::append_hole: { const auto ok = require(1U); if (!ok) return ok.error(); break; }
        case OpCode::enumerate_keys:
        case OpCode::iterator_complete:
        case OpCode::iterator_value:
        case OpCode::to_object: { const auto ok = require(1U); if (!ok) return ok.error(); break; }
        case OpCode::get_iterator: { const auto ok = require(1U); if (!ok) return ok.error(); ++depth; break; }
        case OpCode::iterator_next: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::copy_object_rest:
        case OpCode::copy_data_properties: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::define_property:
        case OpCode::define_getter: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::define_element:
        case OpCode::define_getter_element: { const auto ok = require(3U); if (!ok) return ok.error(); depth -= 2U; break; }
        case OpCode::get_property: { const auto ok = require(1U); if (!ok) return ok.error(); break; }
        case OpCode::set_property:
        case OpCode::set_property_strict: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::get_element: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::get_element_reference: { const auto ok = require(2U); if (!ok) return ok.error(); /* object,key -> converted-key,value */ break; }
        case OpCode::set_element:
        case OpCode::set_element_strict: { const auto ok = require(3U); if (!ok) return ok.error(); depth -= 2U; break; }
        case OpCode::resolve_dynamic_ref:
        case OpCode::release_dynamic_ref:
        case OpCode::reset_local: case OpCode::clone_local_binding: break;
        case OpCode::put_dynamic_ref: { const auto ok = require(1U); if (!ok) return ok.error(); break; }
        case OpCode::set_local:
        case OpCode::initialize_local:
        case OpCode::set_upvalue:
        case OpCode::set_module:
        case OpCode::set_name:
        case OpCode::set_name_strict: { const auto ok = require(1U); if (!ok) return ok.error(); break; }
        case OpCode::pop: { const auto ok = require(1U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::negate:
        case OpCode::positive:
        case OpCode::logical_not:
        case OpCode::bitwise_not:
        case OpCode::typeof_:
        case OpCode::is_nullish: { const auto ok = require(1U); if (!ok) return ok.error(); break; }
        case OpCode::delete_property:
        case OpCode::delete_property_strict: { const auto ok = require(1U); if (!ok) return ok.error(); break; }
        case OpCode::delete_element:
        case OpCode::delete_element_strict: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::add:
        case OpCode::subtract:
        case OpCode::multiply:
        case OpCode::divide:
        case OpCode::remainder:
        case OpCode::exponentiate:
        case OpCode::less:
        case OpCode::less_equal:
        case OpCode::greater:
        case OpCode::greater_equal:
        case OpCode::equal:
        case OpCode::not_equal:
        case OpCode::strict_equal:
        case OpCode::strict_not_equal:
        case OpCode::bitwise_and:
        case OpCode::bitwise_or:
        case OpCode::bitwise_xor:
        case OpCode::shift_left:
        case OpCode::shift_right:
        case OpCode::shift_right_unsigned:
        case OpCode::in_operator:
        case OpCode::instanceof_operator: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::jump_if_false: { const auto ok = require(1U); if (!ok) return ok.error(); --depth; const auto r=enqueue(static_cast<std::size_t>(instruction.operand), depth); if(!r) return r.error(); break; }
        case OpCode::jump:
        case OpCode::break_:
        case OpCode::continue_: break;
        case OpCode::call:
        case OpCode::construct: {
            const std::size_t argc = static_cast<std::size_t>(instruction.operand);
            if (argc == std::numeric_limits<std::size_t>::max()) return verification_error(instruction_pc, "CALL/CONSTRUCT argument count overflow");
            const auto ok = require(argc + 1U); if (!ok) return ok.error(); depth -= argc; break;
        }
        case OpCode::call_method: {
            const std::size_t argc = static_cast<std::size_t>(instruction.operand2);
            if (argc == std::numeric_limits<std::size_t>::max()) return verification_error(instruction_pc, "CALL_METHOD argument count overflow");
            const auto ok = require(argc + 1U); if (!ok) return ok.error(); depth -= argc; break;
        }
        case OpCode::call_element: {
            const std::size_t argc = static_cast<std::size_t>(instruction.operand);
            if (argc == std::numeric_limits<std::size_t>::max()) return verification_error(instruction_pc, "CALL_ELEMENT argument count overflow");
            const auto ok = require(argc + 2U); if (!ok) return ok.error(); depth -= argc + 1U; break;
        }
        case OpCode::call_with_this: {
            const std::size_t argc = static_cast<std::size_t>(instruction.operand);
            if (argc == std::numeric_limits<std::size_t>::max()) return verification_error(instruction_pc, "CALL_WITH_THIS argument count overflow");
            const auto ok = require(argc + 2U); if (!ok) return ok.error(); depth -= argc + 1U; break;
        }
        case OpCode::call_spread:
        case OpCode::construct_spread: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::call_method_spread: { const auto ok = require(2U); if (!ok) return ok.error(); --depth; break; }
        case OpCode::call_element_spread: { const auto ok = require(3U); if (!ok) return ok.error(); depth -= 2U; break; }
        case OpCode::call_with_this_spread: { const auto ok = require(3U); if (!ok) return ok.error(); depth -= 2U; break; }
        case OpCode::throw_: {
            const auto ok = require(1U); if (!ok) return ok.error();
            saw_reachable_terminal = true;
            depth = 0;
            break;
        }
        case OpCode::end_finally:
            break;
        case OpCode::yield_:
            if (depth != 1U) return verification_error(instruction_pc, "YIELD requires exactly one value in the current frame expression stack");
            depth = 1U;
            break;
        case OpCode::return_:
            if (depth != 1U) return verification_error(instruction_pc, "RETURN requires exactly one value in the current frame expression stack");
            saw_reachable_terminal = true;
            depth = 0;
            break;
        }

        if (depth > info.maximum_stack_depth) info.maximum_stack_depth = depth;
        if (instruction.opcode == OpCode::return_ || instruction.opcode == OpCode::throw_) continue;
        if (instruction.opcode == OpCode::jump || instruction.opcode == OpCode::break_ || instruction.opcode == OpCode::continue_) { const auto r=enqueue(static_cast<std::size_t>(instruction.operand), depth); if(!r) return r.error(); continue; }
        const auto r=enqueue(instruction.next_pc, depth); if(!r) return r.error();
    }

    if (!saw_reachable_terminal) return Error{ErrorCode::bytecode_error, "bytecode verification failed: no reachable RETURN or THROW"};
    return info;
}

} // namespace js::bytecode
