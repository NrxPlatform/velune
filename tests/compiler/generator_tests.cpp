#include "test.hpp"

#include <string_view>
#include <js/js.hpp>

namespace {
js::Result<js::Value> eval17(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "test parse failed: " + parsed.diagnostic().message};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}
}

TEST_CASE("generator call creates suspended execution without running body") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval17(context, R"(
        let state = {value: 0};
        function* g() { state.value = 10; yield 1; }
        let it = g();
        state.value
    )");
    REQUIRE(result); REQUIRE(result->as_number() == 0.0);
}

TEST_CASE("generator next resumes at preserved instruction pointer") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval17(context, R"(
        function* g() { yield 10; yield 20; return 30; }
        let it = g();
        let a = it.next(); let b = it.next(); let c = it.next();
        a.value + b.value + c.value
    )");
    REQUIRE(result); REQUIRE(result->as_number() == 60.0);
}

TEST_CASE("generator result done flag distinguishes yield from return") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval17(context, R"(
        function* g() { yield 1; return 2; }
        let it = g(); let a = it.next(); let b = it.next();
        ({a: a, b: b})
    )");
    REQUIRE(result);
    auto a = context.get_property(*result, "a"); REQUIRE(a); auto b = context.get_property(*result, "b"); REQUIRE(b);
    auto ad = context.get_property(*a, "done"); REQUIRE(ad); REQUIRE(!ad->as_boolean());
    auto bd = context.get_property(*b, "done"); REQUIRE(bd); REQUIRE(bd->as_boolean());
}

TEST_CASE("completed generator remains completed") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval17(context, R"(
        function* g() { return 7; }
        let it = g(); let a = it.next(); let b = it.next();
        ({a: a, b: b})
    )");
    REQUIRE(result);
    auto a = context.get_property(*result, "a"); REQUIRE(a); auto b = context.get_property(*result, "b"); REQUIRE(b);
    auto av = context.get_property(*a, "value"); REQUIRE(av); REQUIRE(av->as_number() == 7.0);
    auto bd = context.get_property(*b, "done"); REQUIRE(bd); REQUIRE(bd->as_boolean());
    auto bv = context.get_property(*b, "value"); REQUIRE(bv); REQUIRE(bv->is_undefined());
}

TEST_CASE("generator locals survive suspension") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval17(context, R"(
        function* counter() { let x = 40; yield x; x = x + 2; return x; }
        let it = counter(); let a = it.next(); let b = it.next();
        a.value + b.value
    )");
    REQUIRE(result); REQUIRE(result->as_number() == 82.0);
}

TEST_CASE("closures captured by suspended generator keep live generator locals") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval17(context, R"(
        function* g() {
            let x = 1;
            function inc() { x = x + 1; return x; }
            yield inc();
            yield inc();
            return x;
        }
        let it=g(); let a=it.next(); let b=it.next(); let c=it.next();
        a.value + b.value + c.value
    )");
    REQUIRE(result); REQUIRE(result->as_number() == 8.0);
}

TEST_CASE("generator exception handlers survive suspension") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval17(context, R"(
        function* g() {
            try { yield 1; throw 9; }
            catch (e) { yield e; }
            return 3;
        }
        let it=g(); let a=it.next(); let b=it.next(); let c=it.next();
        a.value + b.value + c.value
    )");
    REQUIRE(result); REQUIRE(result->as_number() == 13.0);
}

TEST_CASE("suspended generator is traced by garbage collector") {
    js::Runtime runtime; js::Context context(runtime);
    auto generator = eval17(context, R"(
        function* g() { let held = {answer: 42}; yield held; return held.answer; }
        g()
    )");
    REQUIRE(generator);
    auto root = runtime.root(*generator); REQUIRE(root);
    (void)runtime.collect_garbage();
    js::VM vm(context);
    auto first = vm.resume_generator(root->value()); REQUIRE(first);
    auto held = context.get_property(*first, "value"); REQUIRE(held);
    auto answer = context.get_property(*held, "answer"); REQUIRE(answer); REQUIRE(answer->as_number() == 42.0);
    (void)runtime.collect_garbage();
    auto second = vm.resume_generator(root->value()); REQUIRE(second);
    auto value = context.get_property(*second, "value"); REQUIRE(value); REQUIRE(value->as_number() == 42.0);
}

TEST_CASE("generator objects compose for of protocol") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval17(context, R"(
        function* values() { yield 2; yield 4; yield 6; }
        let sum = 0;
        for (let x of values()) { sum = sum + x; }
        sum
    )");
    REQUIRE(result); REQUIRE(result->as_number() == 12.0);
}
