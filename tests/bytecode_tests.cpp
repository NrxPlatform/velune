#include "test.hpp"

#include <string>

#include <js/js.hpp>

TEST_CASE("bytecode builder stores constants and encoded instructions") {
    js::bytecode::BytecodeBuilder builder;
    const auto zero = builder.add_constant(js::Value::number(2));
    REQUIRE(zero);
    REQUIRE(*zero == 0U);
    builder.emit_constant(*zero);
    builder.emit(js::bytecode::OpCode::return_);

    const auto chunk = std::move(builder).finish();
    REQUIRE(chunk.constant_count() == 1U);
    REQUIRE(chunk.code_size() == 6U);
}

TEST_CASE("verifier computes instruction count and maximum stack depth") {
    js::bytecode::BytecodeBuilder builder;
    const auto two = builder.add_constant(js::Value::number(2));
    const auto three = builder.add_constant(js::Value::number(3));
    REQUIRE(two && three);
    builder.emit_constant(*two);
    builder.emit_constant(*three);
    builder.emit(js::bytecode::OpCode::multiply);
    builder.emit(js::bytecode::OpCode::return_);

    const auto chunk = std::move(builder).finish();
    js::bytecode::BytecodeVerifier verifier;
    const auto info = verifier.verify(chunk);
    REQUIRE(info);
    REQUIRE(info->instruction_count == 4U);
    REQUIRE(info->maximum_stack_depth == 2U);
}

TEST_CASE("verifier rejects stack underflow") {
    js::bytecode::BytecodeBuilder builder;
    builder.emit(js::bytecode::OpCode::add);
    builder.emit(js::bytecode::OpCode::return_);
    const auto chunk = std::move(builder).finish();

    js::bytecode::BytecodeVerifier verifier;
    const auto result = verifier.verify(chunk);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::bytecode_error);
}

TEST_CASE("verifier rejects invalid constant index") {
    js::bytecode::BytecodeBuilder builder;
    builder.emit_constant(99U);
    builder.emit(js::bytecode::OpCode::return_);
    const auto chunk = std::move(builder).finish();

    js::bytecode::BytecodeVerifier verifier;
    const auto result = verifier.verify(chunk);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::bytecode_error);
}

TEST_CASE("disassembler prints offsets opcodes and constants") {
    js::bytecode::BytecodeBuilder builder;
    const auto constant = builder.add_constant(js::Value::number(42));
    REQUIRE(constant);
    builder.emit_constant(*constant);
    builder.emit(js::bytecode::OpCode::return_);
    const auto chunk = std::move(builder).finish();

    js::bytecode::Disassembler disassembler;
    const auto text = disassembler.disassemble(chunk);
    REQUIRE(text);
    REQUIRE(text->find("0000  CONSTANT 0  ; 42") != std::string::npos);
    REQUIRE(text->find("0005  RETURN") != std::string::npos);
}

TEST_CASE("verifier validates upvalue operands against function metadata") {
    js::bytecode::BytecodeBuilder builder;
    builder.set_upvalues({js::bytecode::UpvalueDescriptor{js::bytecode::UpvalueSource::local, 0U}});
    builder.emit_upvalue(js::bytecode::OpCode::get_upvalue, 1U);
    builder.emit(js::bytecode::OpCode::return_);
    auto chunk = std::move(builder).finish();

    js::bytecode::BytecodeVerifier verifier;
    const auto result = verifier.verify(chunk);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::bytecode_error);
}

TEST_CASE("verifier rejects CLOSURE constants that are not functions") {
    js::bytecode::BytecodeBuilder builder;
    const auto constant = builder.add_constant(js::Value::number(1.0));
    REQUIRE(constant);
    builder.emit_closure(*constant);
    builder.emit(js::bytecode::OpCode::return_);
    auto chunk = std::move(builder).finish();

    js::bytecode::BytecodeVerifier verifier;
    const auto result = verifier.verify(chunk);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::bytecode_error);
}

TEST_CASE("verifier rejects non-string property-key constants") {
    js::bytecode::BytecodeBuilder builder;
    const auto key = builder.add_constant(js::Value::number(1.0));
    REQUIRE(key);
    builder.emit(js::bytecode::OpCode::new_object);
    builder.emit_property(js::bytecode::OpCode::get_property, *key);
    builder.emit(js::bytecode::OpCode::return_);

    js::bytecode::BytecodeVerifier verifier;
    const auto result = verifier.verify(std::move(builder).finish());
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::bytecode_error);
}

TEST_CASE("verifier models object literal property stack effect") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::bytecode::BytecodeBuilder builder;
    const auto key = builder.add_constant(context.string("x"));
    const auto value = builder.add_constant(js::Value::number(7.0));
    REQUIRE(key && value);
    builder.emit(js::bytecode::OpCode::new_object);
    builder.emit_constant(*value);
    builder.emit_property(js::bytecode::OpCode::define_property, *key);
    builder.emit(js::bytecode::OpCode::return_);

    js::bytecode::BytecodeVerifier verifier;
    const auto info = verifier.verify(std::move(builder).finish());
    REQUIRE(info);
    REQUIRE(info->maximum_stack_depth == 2U);
}
