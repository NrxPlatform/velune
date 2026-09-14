#include "test.hpp"

#include <string_view>
#include <js/js.hpp>

namespace {
js::Result<js::Value> eval_p5(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "test parse failed"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}
}

TEST_CASE("atom table interns identical property names") {
    js::Runtime runtime; js::Context context(runtime);
    const auto a = context.property_key("answer");
    const auto b = context.property_key("answer");
    const auto c = context.property_key("other");
    REQUIRE(a.is_atom()); REQUIRE(a == b); REQUIRE(!(a == c));
}

TEST_CASE("string and symbol property keys are distinct") {
    js::Runtime runtime; js::Context context(runtime);
    const auto object = context.object();
    const auto symbol = context.symbol(std::string("x"));
    REQUIRE(context.set_own_property(object, "x", context.number(1)));
    REQUIRE(context.set_own_property(object, js::PropertyKey::symbol(symbol.as_symbol_id()), context.number(2)));
    const auto string_value = context.get_property(object, "x");
    const auto symbol_value = context.get_property(object, js::PropertyKey::symbol(symbol.as_symbol_id()));
    REQUIRE(string_value && string_value->as_number() == 1);
    REQUIRE(symbol_value && symbol_value->as_number() == 2);
}

TEST_CASE("Symbol calls create distinct primitive identities") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval_p5(context, "Symbol('x') === Symbol('x')");
    REQUIRE(result); REQUIRE(result->is_boolean()); REQUIRE(!result->as_boolean());
}

TEST_CASE("Symbol for and keyFor use runtime registry identity") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval_p5(context, "let a=Symbol.for('x'); let b=Symbol.for('x'); (a===b) && (Symbol.keyFor(a)==='x')");
    REQUIRE(result); REQUIRE(result->is_boolean()); REQUIRE(result->as_boolean());
}

TEST_CASE("symbol keyed computed property round trips") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval_p5(context, "let s=Symbol('k'); let o={}; o[s]=42; o[s]");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 42);
}

TEST_CASE("well known Symbol iterator identity is runtime stable") {
    js::Runtime runtime; js::Context first(runtime); js::Context second(runtime);
    const auto a = first.well_known_symbol("iterator");
    const auto b = second.well_known_symbol("iterator");
    REQUIRE(a.is_symbol()); REQUIRE(a.same_heap_identity(b));
}

TEST_CASE("array iteration is keyed by Symbol iterator not string iterator") {
    js::Runtime runtime; js::Context context(runtime);
    const auto array = context.array();
    const auto symbol_method = context.get_property(array, js::PropertyKey::symbol(context.well_known_symbol("iterator").as_symbol_id()));
    const auto string_method = context.get_property(array, "iterator");
    REQUIRE(symbol_method && symbol_method->is_function());
    REQUIRE(string_method && string_method->is_undefined());
}
