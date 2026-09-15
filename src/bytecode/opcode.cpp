#include <js/bytecode/opcode.hpp>

namespace js::bytecode {

std::string_view opcode_name(OpCode opcode) noexcept {
    switch (opcode) {
    case OpCode::constant: return "CONSTANT";
    case OpCode::undefined: return "UNDEFINED";
    case OpCode::get_local: return "GET_LOCAL";
    case OpCode::set_local: return "SET_LOCAL";
    case OpCode::initialize_local: return "INITIALIZE_LOCAL";
    case OpCode::reset_local: return "RESET_LOCAL";
    case OpCode::clone_local_binding: return "CLONE_LOCAL_BINDING";
    case OpCode::get_upvalue: return "GET_UPVALUE";
    case OpCode::get_module: return "GET_MODULE";
    case OpCode::get_name: return "GET_NAME";
    case OpCode::get_name_or_undefined: return "GET_NAME_OR_UNDEFINED";
    case OpCode::get_this: return "GET_THIS";
    case OpCode::get_argument: return "GET_ARGUMENT";
    case OpCode::rest_arguments: return "REST_ARGUMENTS";
    case OpCode::set_upvalue: return "SET_UPVALUE";
    case OpCode::set_module: return "SET_MODULE";
    case OpCode::set_name: return "SET_NAME";
    case OpCode::set_name_strict: return "SET_NAME_STRICT";
    case OpCode::closure: return "CLOSURE";
    case OpCode::new_object: return "NEW_OBJECT";
    case OpCode::new_array: return "NEW_ARRAY";
    case OpCode::append_element: return "APPEND_ELEMENT";
    case OpCode::append_hole: return "APPEND_HOLE";
    case OpCode::append_spread: return "APPEND_SPREAD";
    case OpCode::enumerate_keys: return "ENUMERATE_KEYS";
    case OpCode::to_object: return "TO_OBJECT";
    case OpCode::copy_object_rest: return "COPY_OBJECT_REST";
    case OpCode::copy_data_properties: return "COPY_DATA_PROPERTIES";
    case OpCode::define_property: return "DEFINE_PROPERTY";
    case OpCode::define_element: return "DEFINE_ELEMENT";
    case OpCode::define_getter: return "DEFINE_GETTER";
    case OpCode::define_getter_element: return "DEFINE_GETTER_ELEMENT";
    case OpCode::get_property: return "GET_PROPERTY";
    case OpCode::set_property: return "SET_PROPERTY";
    case OpCode::get_element: return "GET_ELEMENT";
    case OpCode::set_element: return "SET_ELEMENT";
    case OpCode::pop: return "POP";
    case OpCode::add: return "ADD";
    case OpCode::subtract: return "SUBTRACT";
    case OpCode::multiply: return "MULTIPLY";
    case OpCode::divide: return "DIVIDE";
    case OpCode::remainder: return "REMAINDER";
    case OpCode::exponentiate: return "EXPONENTIATE";
    case OpCode::less: return "LESS";
    case OpCode::less_equal: return "LESS_EQUAL";
    case OpCode::greater: return "GREATER";
    case OpCode::greater_equal: return "GREATER_EQUAL";
    case OpCode::equal: return "EQUAL";
    case OpCode::not_equal: return "NOT_EQUAL";
    case OpCode::strict_equal: return "STRICT_EQUAL";
    case OpCode::strict_not_equal: return "STRICT_NOT_EQUAL";
    case OpCode::bitwise_and: return "BITWISE_AND";
    case OpCode::bitwise_or: return "BITWISE_OR";
    case OpCode::bitwise_xor: return "BITWISE_XOR";
    case OpCode::shift_left: return "SHIFT_LEFT";
    case OpCode::shift_right: return "SHIFT_RIGHT";
    case OpCode::shift_right_unsigned: return "SHIFT_RIGHT_UNSIGNED";
    case OpCode::in_operator: return "IN";
    case OpCode::instanceof_operator: return "INSTANCEOF";
    case OpCode::negate: return "NEGATE";
    case OpCode::positive: return "POSITIVE";
    case OpCode::logical_not: return "LOGICAL_NOT";
    case OpCode::bitwise_not: return "BITWISE_NOT";
    case OpCode::typeof_: return "TYPEOF";
    case OpCode::is_nullish: return "IS_NULLISH";
    case OpCode::delete_property: return "DELETE_PROPERTY";
    case OpCode::delete_property_strict: return "DELETE_PROPERTY_STRICT";
    case OpCode::delete_element: return "DELETE_ELEMENT";
    case OpCode::delete_element_strict: return "DELETE_ELEMENT_STRICT";
    case OpCode::jump_if_false: return "JUMP_IF_FALSE";
    case OpCode::jump: return "JUMP";
    case OpCode::break_: return "BREAK";
    case OpCode::continue_: return "CONTINUE";
    case OpCode::call: return "CALL";
    case OpCode::construct: return "CONSTRUCT";
    case OpCode::call_method: return "CALL_METHOD";
    case OpCode::call_element: return "CALL_ELEMENT";
    case OpCode::call_with_this: return "CALL_WITH_THIS";
    case OpCode::call_spread: return "CALL_SPREAD";
    case OpCode::construct_spread: return "CONSTRUCT_SPREAD";
    case OpCode::call_method_spread: return "CALL_METHOD_SPREAD";
    case OpCode::call_element_spread: return "CALL_ELEMENT_SPREAD";
    case OpCode::call_with_this_spread: return "CALL_WITH_THIS_SPREAD";
    case OpCode::throw_: return "THROW";
    case OpCode::end_finally: return "END_FINALLY";
    case OpCode::yield_: return "YIELD";
    case OpCode::return_: return "RETURN";
    }
    return "UNKNOWN";
}

} // namespace js::bytecode
