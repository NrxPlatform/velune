#include "test.hpp"

#include <cmath>
#include <string_view>

#include <js/js.hpp>

namespace {

js::Value normal_value(const js::ExecutionResult& result) {
    REQUIRE(result);
    REQUIRE(result.completion().is_normal());
    return result.completion().value();
}

js::Result<js::Value> eval(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "test source failed to parse"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}

js::ExecutionResult primitive_seven(js::Context& c, js::Value, std::span<const js::Value>) {
    return js::Completion::normal(c.number(7));
}

js::ExecutionResult coercion_throw(js::Context& c, js::Value, std::span<const js::Value>) {
    return js::Completion::throw_(c.string("coercion boom"));
}

} // namespace

TEST_CASE("ToBoolean follows primitive truthiness") {
    js::Runtime runtime; js::Context context(runtime);
    REQUIRE(!js::abstract_operations::to_boolean(context.undefined()));
    REQUIRE(!js::abstract_operations::to_boolean(context.null()));
    REQUIRE(!js::abstract_operations::to_boolean(context.boolean(false)));
    REQUIRE(!js::abstract_operations::to_boolean(context.number(0)));
    REQUIRE(!js::abstract_operations::to_boolean(context.number(std::nan(""))));
    REQUIRE(!js::abstract_operations::to_boolean(context.string("")));
    REQUIRE(js::abstract_operations::to_boolean(context.number(1)));
    REQUIRE(js::abstract_operations::to_boolean(context.object()));
}

TEST_CASE("ToNumber converts primitive values") {
    js::Runtime runtime; js::Context context(runtime);
    REQUIRE(normal_value(js::abstract_operations::to_number(context, context.string("   "))).as_number() == 0.0);
    REQUIRE(normal_value(js::abstract_operations::to_number(context, context.string(" 12.5 "))).as_number() == 12.5);
    REQUIRE(std::isnan(normal_value(js::abstract_operations::to_number(context, context.string("12x"))).as_number()));
    REQUIRE(normal_value(js::abstract_operations::to_number(context, context.null())).as_number() == 0.0);
    REQUIRE(normal_value(js::abstract_operations::to_number(context, context.boolean(true))).as_number() == 1.0);
    REQUIRE(std::isnan(normal_value(js::abstract_operations::to_number(context, context.undefined())).as_number()));
}

TEST_CASE("ToString produces JavaScript primitive spellings") {
    js::Runtime runtime; js::Context context(runtime);
    REQUIRE(normal_value(js::abstract_operations::to_string(context, context.number(-0.0))).as_string() == "0");
    REQUIRE(normal_value(js::abstract_operations::to_string(context, context.number(std::nan("")))).as_string() == "NaN");
    REQUIRE(normal_value(js::abstract_operations::to_string(context, context.number(INFINITY))).as_string() == "Infinity");
    REQUIRE(normal_value(js::abstract_operations::to_string(context, context.boolean(true))).as_string() == "true");
}

TEST_CASE("equality distinguishes strict coercing SameValue and SameValueZero") {
    js::Runtime runtime; js::Context context(runtime);
    auto strict = js::abstract_operations::strict_equal(context, context.number(1), context.string("1"));
    auto loose = js::abstract_operations::abstract_equal(context, context.number(1), context.string("1"));
    auto nullish = js::abstract_operations::abstract_equal(context, context.null(), context.undefined());
    auto same_nan = js::abstract_operations::same_value(context, context.number(std::nan("")), context.number(std::nan("")));
    auto signed_zero = js::abstract_operations::same_value(context, context.number(0.0), context.number(-0.0));
    auto zero = js::abstract_operations::same_value_zero(context, context.number(0.0), context.number(-0.0));
    REQUIRE(strict && !*strict);
    REQUIRE(normal_value(loose).as_boolean());
    REQUIRE(normal_value(nullish).as_boolean());
    REQUIRE(same_nan && *same_nan);
    REQUIRE(signed_zero && !*signed_zero);
    REQUIRE(zero && *zero);
}

TEST_CASE("ToPrimitive honors Symbol.toPrimitive and preserves JavaScript throw") {
    js::Runtime runtime; js::Context context(runtime);
    js::Value object = context.object();
    const auto symbol_key = context.property_key(context.well_known_symbol("toPrimitive"));
    REQUIRE(symbol_key);
    REQUIRE(context.set_own_property(object, *symbol_key, context.native_function("toPrimitive", 1, primitive_seven)));
    REQUIRE(normal_value(js::abstract_operations::to_number(context, object)).as_number() == 7.0);

    REQUIRE(context.set_own_property(object, *symbol_key, context.native_function("throwingToPrimitive", 1, coercion_throw)));
    const auto thrown = js::abstract_operations::to_number(context, object);
    REQUIRE(thrown);
    REQUIRE(thrown.completion().is_throw());
    REQUIRE(thrown.completion().value().as_string() == "coercion boom");
}

TEST_CASE("OrdinaryToPrimitive uses valueOf then toString for number hint") {
    js::Runtime runtime; js::Context context(runtime);
    js::Value object = context.object();
    REQUIRE(context.set_own_property(object, "valueOf", context.native_function("valueOf", 0, primitive_seven)));
    REQUIRE(normal_value(js::abstract_operations::to_number(context, object)).as_number() == 7.0);
}

TEST_CASE("ToNumber accepts non-decimal integer string prefixes") {
    js::Runtime runtime; js::Context context(runtime);
    REQUIRE(normal_value(js::abstract_operations::to_number(context, context.string("0x10"))).as_number() == 16.0);
    REQUIRE(normal_value(js::abstract_operations::to_number(context, context.string("0b101"))).as_number() == 5.0);
    REQUIRE(normal_value(js::abstract_operations::to_number(context, context.string("0o10"))).as_number() == 8.0);
}

TEST_CASE("computed property access uses completion-aware ToPropertyKey") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value object = context.object();
    REQUIRE(context.set_element(object, context.number(12), context.string("twelve")));
    const auto value = context.get_property(object, "12");
    REQUIRE(value && value->is_string() && value->as_string() == "twelve");
}

TEST_CASE("integer and length conversions follow ECMAScript numeric rules") {
    js::Runtime runtime; js::Context context(runtime);
    REQUIRE(normal_value(js::abstract_operations::to_uint32(context, context.number(4294967297.0))).as_number() == 1.0);
    REQUIRE(normal_value(js::abstract_operations::to_uint32(context, context.number(-1.0))).as_number() == 4294967295.0);
    REQUIRE(normal_value(js::abstract_operations::to_int32(context, context.number(4294967295.0))).as_number() == -1.0);
    REQUIRE(normal_value(js::abstract_operations::to_uint8_clamp(context, context.number(2.5))).as_number() == 2.0);
    REQUIRE(normal_value(js::abstract_operations::to_uint8_clamp(context, context.number(3.5))).as_number() == 4.0);
    REQUIRE(normal_value(js::abstract_operations::to_length(context, context.number(-3))).as_number() == 0.0);
}

TEST_CASE("ToObject boxes primitives and rejects nullish values with Throw completion") {
    js::Runtime runtime; js::Context context(runtime);
    const auto boxed = js::abstract_operations::to_object(context, context.number(3));
    REQUIRE(boxed && boxed.completion().is_normal() && boxed.completion().value().is_object());
    const auto nullish = js::abstract_operations::to_object(context, context.null());
    REQUIRE(nullish && nullish.completion().is_throw());
}

TEST_CASE("Call Construct and iterator scaffolding use common semantic boundaries") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "function F(){ this.x = 9; } F");
    REQUIRE(result && result->is_function());
    const auto constructed = js::abstract_operations::construct(context, *result);
    REQUIRE(constructed && constructed.completion().is_normal());
    const auto x = context.get_property(constructed.completion().value(), "x");
    REQUIRE(x && x->as_number() == 9.0);

    js::Value array = context.array();
    REQUIRE(context.array_push(array, context.number(4)));
    const auto iterator_value = js::abstract_operations::get_iterator(context, array);
    REQUIRE(iterator_value && iterator_value.completion().is_normal());
    js::IteratorRecord record{iterator_value.completion().value(), context.get_property(iterator_value.completion().value(), "next").value()};
    const auto next = js::abstract_operations::iterator_next(context, record);
    REQUIRE(next && next.completion().is_normal());
    REQUIRE(!normal_value(js::abstract_operations::iterator_complete(context, next.completion().value())).as_boolean());
    REQUIRE(normal_value(js::abstract_operations::iterator_value(context, next.completion().value())).as_number() == 4.0);
}
