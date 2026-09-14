#include "test.hpp"

#include <span>
#include <string_view>

#include <js/js.hpp>

namespace {

js::Result<js::Value> eval(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "source failed to parse"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}

js::ExecutionResult native_add_to_this(js::Context& context, js::Value this_value, std::span<const js::Value> arguments) {
    if (arguments.size() != 1U || !arguments[0].is_number()) {
        return js::EngineFailure{js::Error{js::ErrorCode::type_error, "native_add_to_this expects one number"}};
    }
    const auto base = context.get_property(this_value, "base");
    if (!base) return js::EngineFailure{base.error()};
    if (!base->is_number()) return js::EngineFailure{js::Error{js::ErrorCode::type_error, "this.base must be a number"}};
    return js::Completion::normal(context.number(base->as_number() + arguments[0].as_number()));
}

} // namespace

TEST_CASE("prototype lookup finds inherited property") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto prototype = context.object();
    REQUIRE(context.set_own_property(prototype, "x", context.number(7)));
    const auto child = context.object(prototype);
    REQUIRE(child);
    const auto value = context.get_property(*child, "x");
    REQUIRE(value);
    REQUIRE(value->as_number() == 7.0);
    const auto own = context.get_own_property(*child, "x");
    REQUIRE(own);
    REQUIRE(own->is_undefined());
}

TEST_CASE("prototype lookup walks multiple levels") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto root = context.object();
    REQUIRE(context.set_own_property(root, "x", context.number(3)));
    const auto middle = context.object(root); REQUIRE(middle);
    const auto leaf = context.object(*middle); REQUIRE(leaf);
    const auto value = context.get_property(*leaf, "x");
    REQUIRE(value);
    REQUIRE(value->as_number() == 3.0);
}

TEST_CASE("own property shadows inherited property") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto prototype = context.object();
    REQUIRE(context.set_own_property(prototype, "x", context.number(1)));
    const auto child = context.object(prototype); REQUIRE(child);
    REQUIRE(context.set_own_property(*child, "x", context.number(9)));
    const auto value = context.get_property(*child, "x");
    REQUIRE(value);
    REQUIRE(value->as_number() == 9.0);
}

TEST_CASE("assignment through prototype creates receiver own property") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto prototype = context.object();
    REQUIRE(context.set_own_property(prototype, "x", context.number(1)));
    const auto child = context.object(prototype); REQUIRE(child);
    REQUIRE(context.set_property(*child, "x", context.number(5)));
    const auto child_value = context.get_property(*child, "x");
    const auto prototype_value = context.get_property(prototype, "x");
    REQUIRE(child_value && prototype_value);
    REQUIRE(child_value->as_number() == 5.0);
    REQUIRE(prototype_value->as_number() == 1.0);
}

TEST_CASE("prototype cycles are rejected") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto a = context.object();
    const auto b = context.object();
    REQUIRE(context.set_prototype(a, b));
    const auto cycle = context.set_prototype(b, a);
    REQUIRE(!cycle);
    REQUIRE(cycle.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("this expression is undefined at program level") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "this");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("method call binds receiver as this") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context,
        "function add(n) { this.x = this.x + n; return this.x; } "
        "let o = {x: 10, add: add}; o.add(5)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 15.0);
}

TEST_CASE("detached non-strict function call uses global this rather than method receiver") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context,
        "function read() { return this.x; } "
        "let o = {x: 10, read: read}; let f = o.read; f()");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("native method receives receiver through calling convention") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto prototype = context.object();
    const auto native = context.native_function("addToThis", 1, &native_add_to_this);
    REQUIRE(context.set_own_property(prototype, "add", native));
    const auto child = context.object(prototype); REQUIRE(child);
    REQUIRE(context.set_own_property(*child, "base", context.number(40)));

    js::bytecode::BytecodeBuilder builder;
    const auto object_constant = builder.add_constant(*child); REQUIRE(object_constant);
    const auto argument_constant = builder.add_constant(context.number(2)); REQUIRE(argument_constant);
    const auto key_constant = builder.add_constant(context.string("add")); REQUIRE(key_constant);
    builder.emit_constant(*object_constant);
    builder.emit_constant(*argument_constant);
    builder.emit_method_call(*key_constant, 1);
    builder.emit(js::bytecode::OpCode::return_);
    auto chunk = std::move(builder).finish();

    js::VM vm(context);
    const auto result = vm.run(chunk);
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 42.0);
}

TEST_CASE("computed method call preserves reference receiver") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context,
        "function read() { return this.x; } "
        "let o = {x: 41, read: read}; let k = 'read'; o[k]()");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 41.0);
}

TEST_CASE("computed method reference evaluates key once") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context,
        "let count = 0; "
        "function key() { count++; return 'read'; } "
        "function read() { return this.x; } "
        "let o = {x: 7, read: read}; let value = o[key()](); count * 10 + value");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 17.0);
}

TEST_CASE("verifier rejects CALL_METHOD with non-string property key") {
    js::bytecode::BytecodeBuilder builder;
    const auto receiver = builder.add_constant(js::Value::undefined()); REQUIRE(receiver);
    const auto bad_key = builder.add_constant(js::Value::number(1)); REQUIRE(bad_key);
    builder.emit_constant(*receiver);
    builder.emit_method_call(*bad_key, 0);
    builder.emit(js::bytecode::OpCode::return_);
    const auto chunk = std::move(builder).finish();
    js::bytecode::BytecodeVerifier verifier;
    const auto result = verifier.verify(chunk);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::bytecode_error);
}

TEST_CASE("verifier rejects CALL_METHOD stack underflow") {
    js::Runtime runtime;
    js::Context context(runtime);
    js::bytecode::BytecodeBuilder builder;
    const auto key = builder.add_constant(context.string("m")); REQUIRE(key);
    builder.emit_method_call(*key, 0);
    builder.emit(js::bytecode::OpCode::return_);
    const auto chunk = std::move(builder).finish();
    js::bytecode::BytecodeVerifier verifier;
    const auto result = verifier.verify(chunk);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::bytecode_error);
}


TEST_CASE("verifier rejects CALL_ELEMENT stack underflow") {
    js::bytecode::BytecodeBuilder builder;
    builder.emit_element_call(0);
    builder.emit(js::bytecode::OpCode::return_);
    const auto chunk = std::move(builder).finish();
    js::bytecode::BytecodeVerifier verifier;
    const auto result = verifier.verify(chunk);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::bytecode_error);
}
