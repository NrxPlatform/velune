#include "test.hpp"
#include <string_view>
#include <js/js.hpp>

namespace {
js::Result<js::Value> eval18(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "test parse failed: " + parsed.diagnostic().message};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}
}

TEST_CASE("regexp literal compiles to regexp object") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval18(context, R"(let re=/ab/; re.test("xxabyy"))");
    REQUIRE(result); REQUIRE(result->as_boolean());
}
TEST_CASE("regexp literal distinguishes division from expression-start slash") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval18(context, R"(let x=8/2; let re=/4/; if (re.test("4")) { x = x + 1; } x)");
    REQUIRE(result); REQUIRE(result->as_number() == 5.0);
}
TEST_CASE("regexp ignore-case flag works") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval18(context, R"(/hello/i.test("HeLLo"))");
    REQUIRE(result); REQUIRE(result->as_boolean());
}
TEST_CASE("regexp exec returns captures index and input") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval18(context, R"(let m=/(a)(b)/.exec("xxabyy"); m.index + m[0].length + m[1].length + m[2].length)");
    REQUIRE(result); REQUIRE(result->as_number() == 6.0);
}
TEST_CASE("regexp exec returns null when unmatched") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval18(context, R"(/z/.exec("abc"))");
    REQUIRE(result); REQUIRE(result->is_null());
}
TEST_CASE("regexp global flag advances last index across calls") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval18(context, R"(let re=/a/g; let a=re.exec("a-a"); let b=re.exec("a-a"); a.index + b.index)");
    REQUIRE(result); REQUIRE(result->as_number() == 2.0);
}
TEST_CASE("regexp global failure resets search position") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval18(context, R"(let re=/a/g; re.exec("a"); re.exec("a"); let again=re.exec("a"); again.index)");
    REQUIRE(result); REQUIRE(result->as_number() == 0.0);
}
TEST_CASE("unsupported regexp flags are compile errors") {
    js::Runtime runtime; js::Context context(runtime);
    auto parsed = js::frontend::parse_program(R"(/a/u)"); REQUIRE(parsed);
    auto chunk = js::compiler::compile_program(context, parsed.program());
    REQUIRE(!chunk); REQUIRE(chunk.error().code() == js::ErrorCode::compile_error);
}
TEST_CASE("regexp objects survive gc through ordinary roots") {
    js::Runtime runtime; js::Context context(runtime);
    auto regexp = context.regexp("a+", "i"); REQUIRE(regexp);
    auto root = runtime.root(*regexp); REQUIRE(root);
    (void)runtime.collect_garbage();
    auto tested = context.regexp_test(root->value(), context.string("AAA")); REQUIRE(tested); REQUIRE(tested->as_boolean());
}
