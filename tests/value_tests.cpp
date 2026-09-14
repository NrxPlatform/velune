#include "test.hpp"

#include <cmath>
#include <stdexcept>

#include <js/js.hpp>

TEST_CASE("primitive value tags and payloads") {
    const auto u = js::Value::undefined();
    const auto n = js::Value::null();
    const auto b = js::Value::boolean(true);
    const auto x = js::Value::number(12.5);

    REQUIRE(u.is_undefined());
    REQUIRE(n.is_null());
    REQUIRE(b.is_boolean());
    REQUIRE(b.as_boolean());
    REQUIRE(x.is_number());
    REQUIRE(x.as_number() == 12.5);
}

TEST_CASE("string is heap backed and preserves contents") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto baseline = runtime.heap_cell_count();
    const auto text = context.string("acorn -> bytecode -> vm");
    REQUIRE(text.is_string());
    REQUIRE(text.as_string() == "acorn -> bytecode -> vm");
    REQUIRE(runtime.heap_cell_count() == baseline + 1U);
}

TEST_CASE("debug formatting keeps important number cases visible") {
    REQUIRE(js::Value::undefined().to_debug_string() == "undefined");
    REQUIRE(js::Value::null().to_debug_string() == "null");
    REQUIRE(js::Value::boolean(false).to_debug_string() == "false");
    REQUIRE(js::Value::number(-0.0).to_debug_string() == "-0");
    REQUIRE(js::Value::number(INFINITY).to_debug_string() == "Infinity");
    REQUIRE(js::Value::number(NAN).to_debug_string() == "NaN");
}

TEST_CASE("typed accessors reject the wrong tag") {
    bool threw = false;
    try {
        (void)js::Value::number(1).as_boolean();
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE(threw);
}
