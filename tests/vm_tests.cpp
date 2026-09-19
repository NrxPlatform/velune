#include "test.hpp"

#include <js/js.hpp>

namespace {

js::bytecode::BytecodeChunk arithmetic_chunk(js::Context& context) {
    js::bytecode::BytecodeBuilder builder;
    const auto two = builder.add_constant(context.number(2));
    const auto three = builder.add_constant(context.number(3));
    const auto one = builder.add_constant(context.number(1));
    if (!two || !three || !one) {
        throw std::runtime_error("failed to add test constant");
    }

    builder.emit_constant(*two);
    builder.emit_constant(*three);
    builder.emit(js::bytecode::OpCode::multiply);
    builder.emit_constant(*one);
    builder.emit(js::bytecode::OpCode::add);
    builder.emit(js::bytecode::OpCode::return_);
    return std::move(builder).finish();
}

} // namespace

TEST_CASE("vm executes 2 * 3 + 1") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::VM vm(context);

    const auto chunk = arithmetic_chunk(context);
    const auto result = vm.run(chunk);

    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 7.0);
    REQUIRE(vm.stack_size() == 0U);
}

TEST_CASE("vm preserves operand order for subtraction") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::bytecode::BytecodeBuilder builder;
    const auto ten = builder.add_constant(context.number(10));
    const auto four = builder.add_constant(context.number(4));
    REQUIRE(ten && four);
    builder.emit_constant(*ten);
    builder.emit_constant(*four);
    builder.emit(js::bytecode::OpCode::subtract);
    builder.emit(js::bytecode::OpCode::return_);

    js::VM vm(context);
    const auto result = vm.run(std::move(builder).finish());
    REQUIRE(result);
    REQUIRE(result->as_number() == 6.0);
}

TEST_CASE("vm ADD uses primitive concatenation semantics") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::bytecode::BytecodeBuilder builder;
    const auto text = builder.add_constant(context.string("not a number"));
    const auto one = builder.add_constant(context.number(1));
    REQUIRE(text && one);
    builder.emit_constant(*text);
    builder.emit_constant(*one);
    builder.emit(js::bytecode::OpCode::add);
    builder.emit(js::bytecode::OpCode::return_);

    js::VM vm(context);
    const auto result = vm.run(std::move(builder).finish());
    REQUIRE(result);
    REQUIRE(result->is_string());
    REQUIRE(result->as_string() == "not a number1");
}

TEST_CASE("vm rejects heap constants from another runtime") {
    js::Runtime runtime_a;
    js::Runtime runtime_b;
    js::Context context_a(runtime_a);
    js::Context context_b(runtime_b);

    js::bytecode::BytecodeBuilder builder;
    const auto text = builder.add_constant(context_a.string("owned by A"));
    REQUIRE(text);
    builder.emit_constant(*text);
    builder.emit(js::bytecode::OpCode::return_);

    js::VM vm(context_b);
    const auto result = vm.run(std::move(builder).finish());
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::EngineFailureCode::HostContractViolation);
}

TEST_CASE("vm can return a same-runtime heap-backed constant") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::bytecode::BytecodeBuilder builder;
    const auto text = builder.add_constant(context.string("hello bytecode"));
    REQUIRE(text);
    builder.emit_constant(*text);
    builder.emit(js::bytecode::OpCode::return_);

    js::VM vm(context);
    const auto result = vm.run(std::move(builder).finish());
    REQUIRE(result);
    REQUIRE(result->is_string());
    REQUIRE(result->as_string() == "hello bytecode");
}

TEST_CASE("retained dynamic Reference preserves static fallback across RHS mutation") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::bytecode::BytecodeBuilder builder;
    builder.set_local_count(1U);
    const auto name = builder.add_constant(context.string("retained_local"));
    const auto initial = builder.add_constant(context.number(10));
    const auto changed = builder.add_constant(context.number(30));
    const auto assigned = builder.add_constant(context.number(21));
    REQUIRE(name && initial && changed && assigned);
    builder.emit_constant(*initial);
    builder.emit_local(js::bytecode::OpCode::set_local, 0U);
    builder.emit(js::bytecode::OpCode::pop);
    builder.emit_dynamic_reference(*name, 1U, false);
    builder.emit_constant(*changed);
    builder.emit_local(js::bytecode::OpCode::set_local, 0U);
    builder.emit(js::bytecode::OpCode::pop);
    builder.emit_local(js::bytecode::OpCode::get_dynamic_ref, 0U);
    builder.emit(js::bytecode::OpCode::pop);
    builder.emit_constant(*assigned);
    builder.emit_local(js::bytecode::OpCode::put_dynamic_ref, 0U);
    builder.emit(js::bytecode::OpCode::pop);
    builder.emit_local(js::bytecode::OpCode::get_dynamic_ref, 0U);
    builder.emit_local(js::bytecode::OpCode::release_dynamic_ref, 0U);
    builder.emit(js::bytecode::OpCode::return_);
    js::VM vm(context);
    const auto result = vm.run(std::move(builder).finish());
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 21.0);
}

TEST_CASE("retained dynamic Reference strict unresolvable assignment throws") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::bytecode::BytecodeBuilder builder;
    const auto name = builder.add_constant(context.string("__velune_missing_strict_ref__"));
    const auto value = builder.add_constant(context.number(7));
    REQUIRE(name && value);
    builder.emit_dynamic_reference(*name, 0U, true);
    builder.emit_constant(*value);
    builder.emit_local(js::bytecode::OpCode::put_dynamic_ref, 0U);
    builder.emit(js::bytecode::OpCode::return_);
    js::VM vm(context);
    const auto result = vm.run(std::move(builder).finish());
    REQUIRE(result);
    REQUIRE(result.completion().is_throw());
}

TEST_CASE("retained outer Reference survives a handled inner abrupt completion") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::bytecode::BytecodeBuilder builder;
    builder.set_local_count(1U);
    const auto name = builder.add_constant(context.string("outer_binding"));
    const auto initial = builder.add_constant(context.number(41));
    const auto exception = builder.add_constant(context.number(99));
    REQUIRE(name && initial && exception);
    builder.emit_constant(*initial);
    builder.emit_local(js::bytecode::OpCode::set_local, 0U);
    builder.emit(js::bytecode::OpCode::pop);
    builder.emit_dynamic_reference(*name, 1U, false); // slot 0: outside try
    const auto try_start = static_cast<std::uint32_t>(builder.offset());
    builder.emit_dynamic_reference(*name, 1U, false); // slot 1: abandoned
    builder.emit_constant(*exception);
    builder.emit(js::bytecode::OpCode::throw_);
    const auto try_end = static_cast<std::uint32_t>(builder.offset());
    const auto catch_start = try_end;
    builder.emit(js::bytecode::OpCode::pop); // caught exception
    builder.emit_local(js::bytecode::OpCode::get_dynamic_ref, 0U);
    builder.emit_local(js::bytecode::OpCode::release_dynamic_ref, 0U);
    builder.emit(js::bytecode::OpCode::return_);
    const auto catch_end = static_cast<std::uint32_t>(builder.offset());
    builder.add_exception_handler(js::bytecode::ExceptionHandler{
        try_start, try_end, catch_start, catch_end});
    js::VM vm(context);
    const auto result = vm.run(std::move(builder).finish());
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 41.0);
}
