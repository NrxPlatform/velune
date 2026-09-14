#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {
js::Result<js::Value> eval13(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "test parse failed"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}
}

TEST_CASE("class constructor initializes this and prototype method executes") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context,
        "class Point { constructor(x, y) { this.x = x; this.y = y; } sum() { return this.x + this.y; } } "
        "let p = new Point(2, 3); p.sum()");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("class constructor cannot be called without new") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context, "class A { constructor() {} } A()");
    REQUIRE(!result); REQUIRE(result.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("constructor primitive return is ignored in favor of allocated receiver") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context, "class Box { constructor(x) { this.x = x; return 99; } } let b = new Box(7); b.x");
    REQUIRE(result); REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("constructor object return replaces allocated receiver") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context, "class Box { constructor() { return {x: 9}; } } let b = new Box(); b.x");
    REQUIRE(result); REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("class without explicit constructor receives default constructor") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context, "class A { value() { return 4; } } new A().value()");
    REQUIRE(result); REQUIRE(result->as_number() == 4.0);
}

TEST_CASE("class methods capture surrounding lexical bindings") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context, "let k = 3; class A { value() { return k; } } new A().value()");
    REQUIRE(result); REQUIRE(result->as_number() == 3.0);
}

TEST_CASE("class inside function captures factory invocation state") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context,
        "function make(v) { class A { get() { return v; } } return new A(); } "
        "make(7).get()");
    REQUIRE(result); REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("class prototype exposes constructor function") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context, "class A {} A.prototype.constructor");
    REQUIRE(result); REQUIRE(result->is_function());
}

TEST_CASE("new rejects non function targets") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context, "let x = 1; new x()");
    REQUIRE(!result); REQUIRE(result.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("extends remains an explicit boundary") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval13(context, "class A {} class B extends A {} new B()");
    REQUIRE(!result); REQUIRE(result.error().code() == js::ErrorCode::compile_error);
}

TEST_CASE("GC traces instance prototype and prototype methods") {
    js::Runtime runtime; js::Context context(runtime);
    auto instance = eval13(context, "class A { value() { return 42; } } new A()");
    REQUIRE(instance); REQUIRE(instance->is_object());
    auto root_result = runtime.root(*instance); REQUIRE(root_result);
    auto root = std::move(*root_result);
    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.marked > 0U);
    auto method = context.get_property(root.value(), "value");
    REQUIRE(method); REQUIRE(method->is_function());
}
