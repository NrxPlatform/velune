#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {
js::Result<js::Value> eval16(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "test parse failed"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}
}

TEST_CASE("promise reactions are deferred until the host drains jobs") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval16(context, R"(
        let state = {value: 0};
        function reaction(x) { state.value = state.value + 10; return x + 1; }
        let q = Promise.resolve(41).then(reaction);
        state.value = state.value + 1;
        ({state: state, q: q})
    )");
    REQUIRE(result);
    auto root = runtime.root(*result); REQUIRE(root);
    auto state = context.get_property(*result, "state"); REQUIRE(state);
    auto before = context.get_property(*state, "value"); REQUIRE(before); REQUIRE(before->as_number() == 1.0);
    REQUIRE(runtime.pending_job_count() == 1U);
    auto ran = runtime.run_jobs(context); REQUIRE(ran); REQUIRE(*ran == 1U);
    auto after = context.get_property(*state, "value"); REQUIRE(after); REQUIRE(after->as_number() == 11.0);
    auto q = context.get_property(*result, "q"); REQUIRE(q);
    auto q_result = context.promise_result(*q); REQUIRE(q_result); REQUIRE(q_result->as_number() == 42.0);
}

TEST_CASE("promise then chains schedule later reactions through the same queue") {
    js::Runtime runtime; js::Context context(runtime);
    auto promise = eval16(context, R"(
        function plusOne(x) { return x + 1; }
        Promise.resolve(40).then(plusOne).then(plusOne)
    )");
    REQUIRE(promise);
    auto root = runtime.root(*promise); REQUIRE(root);
    auto ran = runtime.run_jobs(context); REQUIRE(ran); REQUIRE(*ran == 2U);
    auto value = context.promise_result(*promise); REQUIRE(value); REQUIRE(value->as_number() == 42.0);
}

TEST_CASE("promise rejection propagates to catch asynchronously") {
    js::Runtime runtime; js::Context context(runtime);
    auto promise = eval16(context, R"(
        function recover(x) { return x + 10; }
        Promise.reject(5).catch(recover)
    )");
    REQUIRE(promise);
    auto root = runtime.root(*promise); REQUIRE(root);
    auto ran = runtime.run_jobs(context); REQUIRE(ran); REQUIRE(*ran == 1U);
    auto value = context.promise_result(*promise); REQUIRE(value); REQUIRE(value->as_number() == 15.0);
}

TEST_CASE("throwing inside a then callback rejects the chained promise") {
    js::Runtime runtime; js::Context context(runtime);
    auto promise = eval16(context, R"(
        function fail(x) { throw x + 1; }
        function recover(x) { return x + 10; }
        Promise.resolve(1).then(fail).catch(recover)
    )");
    REQUIRE(promise);
    auto root = runtime.root(*promise); REQUIRE(root);
    auto ran = runtime.run_jobs(context); REQUIRE(ran); REQUIRE(*ran == 2U);
    auto value = context.promise_result(*promise); REQUIRE(value); REQUIRE(value->as_number() == 12.0);
}

TEST_CASE("promise resolution adopts a promise returned by a reaction") {
    js::Runtime runtime; js::Context context(runtime);
    auto promise = eval16(context, R"(
        function wrap(x) { return Promise.resolve(x + 2); }
        function plusOne(x) { return x + 1; }
        Promise.resolve(39).then(wrap).then(plusOne)
    )");
    REQUIRE(promise);
    auto root = runtime.root(*promise); REQUIRE(root);
    auto ran = runtime.run_jobs(context); REQUIRE(ran); REQUIRE(*ran == 3U);
    auto value = context.promise_result(*promise); REQUIRE(value); REQUIRE(value->as_number() == 42.0);
}

TEST_CASE("queued promise jobs are garbage collector roots") {
    js::Runtime runtime; js::Context context(runtime);
    auto promise = eval16(context, R"(
        function make(x) { return {value: x + 1}; }
        Promise.resolve(41).then(make)
    )");
    REQUIRE(promise);
    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.after > 0U);
    auto ran = runtime.run_jobs(context); REQUIRE(ran); REQUIRE(*ran == 1U);
    auto value = context.promise_result(*promise); REQUIRE(value);
    auto answer = context.get_property(*value, "value"); REQUIRE(answer); REQUIRE(answer->as_number() == 42.0);
}

TEST_CASE("promise callback jobs preserve FIFO ordering") {
    js::Runtime runtime; js::Context context(runtime);
    auto result = eval16(context, R"(
        let state = {value: 0};
        function first(x) { state.value = state.value * 10 + 1; return x; }
        function second(x) { state.value = state.value * 10 + 2; return x; }
        Promise.resolve(0).then(first);
        Promise.resolve(0).then(second);
        state
    )");
    REQUIRE(result);
    auto root = runtime.root(*result); REQUIRE(root);
    auto ran = runtime.run_jobs(context); REQUIRE(ran); REQUIRE(*ran == 2U);
    auto value = context.get_property(*result, "value"); REQUIRE(value); REQUIRE(value->as_number() == 12.0);
}
