#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {

js::Result<js::Value> eval(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "object test source failed to parse"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}


} // namespace

TEST_CASE("object literal creates readable own properties") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let o = {x: 2, y: 3}; o.x + o.y");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("property assignment mutates object identity") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let o = {x: 1}; let alias = o; alias.x = 9; o.x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("property assignment expression leaves assigned value") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let o = {x: 1}; let y = (o.x = 4); y + o.x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 8.0);
}

TEST_CASE("nested object member reads compose") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let outer = {inner: {value: 7}}; outer.inner.value");
    REQUIRE(result);
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("missing source property produces undefined") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let o = {}; o.missing");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("property access on primitive boxes and resolves through the wrapper prototype") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let x = 1; x.foo");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("computed property access uses element semantics") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "let o = {x: 1}; o[\"x\"]");
    REQUIRE(result); REQUIRE(result->as_number() == 1.0);
}

TEST_CASE("closure can retain and mutate an object through captured binding") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function make() { let o = {x: 1}; function inc() { o.x = o.x + 1; return o.x; } return inc; } let f = make(); f() + f()");
    REQUIRE(result);
    REQUIRE(result->as_number() == 5.0);
}
