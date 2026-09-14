#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {

js::Result<js::Value> eval(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "closure test source failed to parse"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}

js::Result<js::bytecode::BytecodeChunk> compile(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "closure test source failed to parse"};
    return js::compiler::compile_program(context, parsed.program());
}

} // namespace

TEST_CASE("returned closure keeps captured local alive after outer frame returns") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function outer(a) { let x = a; function inner(b) { return x + b; } return inner; } let f = outer(10); f(5)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 15.0);
}

TEST_CASE("closure mutation persists through closed upvalue storage") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function makeCounter() { let x = 0; function inc() { x = x + 1; return x; } return inc; } let c = makeCounter(); c() + c()");
    REQUIRE(result);
    REQUIRE(result->as_number() == 3.0);
}

TEST_CASE("separate outer calls create independent captured cells") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function makeCounter(start) { let x = start; function inc() { x = x + 1; return x; } return inc; } let a = makeCounter(0); let b = makeCounter(10); a() + b()");
    REQUIRE(result);
    REQUIRE(result->as_number() == 12.0);
}

TEST_CASE("sibling closures share one captured binding") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function outer() { let x = 1; function inc() { x = x + 1; return x; } function read() { return x; } inc(); return read; } let read = outer(); read()");
    REQUIRE(result);
    REQUIRE(result->as_number() == 2.0);
}

TEST_CASE("deep closure capture is threaded through intermediate functions") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function outer(a) { function middle(b) { function inner(c) { return a + b + c; } return inner; } return middle; } let m = outer(1); let i = m(2); i(3)");
    REQUIRE(result);
    REQUIRE(result->as_number() == 6.0);
}

TEST_CASE("captured const remains immutable") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto chunk = compile(context, "function outer() { const x = 1; function bad() { x = 2; return x; } return bad; } let bad=outer(); bad();");
    REQUIRE(chunk);
    const auto result = eval(context, "function outer() { const x = 1; function bad() { x = 2; return x; } return bad; } let bad=outer(); bad();");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("unresolved identifier survives recursive capture search until evaluation") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto chunk = compile(context, "function outer() { function inner() { return missing; } return inner; } let inner = outer(); inner()");
    REQUIRE(chunk);
    const auto result = eval(context, "function outer() { function inner() { return missing; } return inner; } let inner = outer(); inner()");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::reference_error);
}
