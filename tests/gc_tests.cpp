#include "test.hpp"

#include <cstdint>
#include <span>
#include <string_view>

#include <js/bytecode/builder.hpp>
#include <js/compiler/compiler.hpp>
#include <js/frontend/frontend.hpp>
#include <js/js.hpp>

namespace {

js::Result<js::bytecode::BytecodeChunk> compile(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::compile_error, parsed.diagnostic().message};
    return js::compiler::compile_program(context, parsed.program());
}

js::ExecutionResult native_collect_and_read(js::Context& context, js::Value, std::span<const js::Value> arguments) {
    if (arguments.size() != 1U) return js::EngineFailure{js::Error{js::ErrorCode::type_error, "collector test expects one argument"}};
    const auto stats = context.runtime().collect_garbage();
    (void)stats;
    return js::execution_from_result(context.get_property(arguments[0], "answer"));
}

js::bytecode::BytecodeChunk call_one(js::Value function, js::Value argument) {
    js::bytecode::BytecodeBuilder builder;
    const auto fn = builder.add_constant(function);
    const auto arg = builder.add_constant(argument);
    if (!fn || !arg) throw std::runtime_error("failed to create test constants");
    builder.emit_constant(*fn);
    builder.emit_constant(*arg);
    builder.emit_call(1);
    builder.emit(js::bytecode::OpCode::return_);
    return std::move(builder).finish();
}

js::bytecode::BytecodeChunk call_zero(js::Value function) {
    js::bytecode::BytecodeBuilder builder;
    const auto fn = builder.add_constant(function);
    if (!fn) throw std::runtime_error("failed to create test function constant");
    builder.emit_constant(*fn);
    builder.emit_call(0);
    builder.emit(js::bytecode::OpCode::return_);
    return std::move(builder).finish();
}

} // namespace

TEST_CASE("garbage collector reclaims unreachable object cycles") {
    js::Runtime runtime;
    js::Context context(runtime);

    (void)context.object_prototype();
    const auto baseline = runtime.heap_cell_count();
    const auto a = context.object();
    const auto b = context.object();
    REQUIRE(context.set_own_property(a, "peer", b));
    REQUIRE(context.set_own_property(b, "peer", a));
    REQUIRE(runtime.heap_cell_count() == baseline + 2U);

    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.before == baseline + 2U);
    REQUIRE(stats.marked == baseline);
    REQUIRE(stats.collected == 2U);
    REQUIRE(stats.after == baseline);
}

TEST_CASE("persistent root keeps an object graph alive") {
    js::Runtime runtime;
    js::Context context(runtime);

    (void)context.object_prototype();
    const auto baseline = runtime.heap_cell_count();
    const auto parent = context.object();
    const auto child = context.object();
    REQUIRE(context.set_own_property(parent, "child", child));

    auto root_result = runtime.root(parent);
    REQUIRE(root_result);
    auto root = std::move(root_result).value();

    auto stats = runtime.collect_garbage();
    REQUIRE(stats.marked == baseline + 2U);
    REQUIRE(stats.collected == 0U);
    REQUIRE(runtime.heap_cell_count() == baseline + 2U);

    const auto property = context.get_property(root.value(), "child");
    REQUIRE(property);
    REQUIRE(property->is_object());

    root.reset();
    stats = runtime.collect_garbage();
    REQUIRE(stats.collected == 2U);
    REQUIRE(runtime.heap_cell_count() == baseline);
}

TEST_CASE("persistent root rejects a heap Value owned by another Runtime") {
    js::Runtime first;
    js::Runtime second;
    js::Context first_context(first);
    js::Context second_context(second);

    const auto foreign = second_context.object();
    const auto root = first.root(foreign);
    REQUIRE(!root);
    REQUIRE(root.error().code() == js::ErrorCode::runtime_mismatch);
}

TEST_CASE("VM operand stack and frame are temporary garbage collection roots") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto object = context.object();
    REQUIRE(context.set_own_property(object, "answer", context.number(42.0)));
    const auto collector = context.native_function("collectAndRead", 1, native_collect_and_read);
    auto chunk = call_one(collector, object);

    js::VM vm(context);
    const auto result = vm.run(chunk);
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 42.0);

    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.collected >= 2U);
}

TEST_CASE("persistent closure roots captured object and closed upvalue") {
    js::Runtime runtime;
    js::Context context(runtime);

    auto chunk = compile(context,
        "function make() {"
        "  let state = {answer: 41};"
        "  function read() { return state.answer + 1; }"
        "  return read;"
        "}"
        "make()"
    );
    REQUIRE(chunk);

    js::VM vm(context);
    const auto closure = vm.run(*chunk);
    REQUIRE(closure);
    REQUIRE(closure->is_function());

    auto root_result = runtime.root(*closure);
    REQUIRE(root_result);
    auto root = std::move(root_result).value();

    const auto before = runtime.heap_cell_count();
    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.marked >= 3U);
    REQUIRE(runtime.heap_cell_count() <= before);

    auto invoke = call_zero(root.value());
    const auto value = vm.run(invoke);
    REQUIRE(value);
    REQUIRE(value->is_number());
    REQUIRE(value->as_number() == 42.0);
}

TEST_CASE("collector handles a deep reachable graph iteratively") {
    js::Runtime runtime;
    js::Context context(runtime);

    (void)context.object_prototype();
    const auto baseline = runtime.heap_cell_count();
    constexpr std::size_t depth = 20000U;
    auto head = context.object();
    auto current = head;
    for (std::size_t index = 1; index < depth; ++index) {
        const auto next = context.object();
        REQUIRE(context.set_own_property(current, "next", next));
        current = next;
    }
    REQUIRE(runtime.heap_cell_count() == baseline + depth);

    auto root_result = runtime.root(head);
    REQUIRE(root_result);
    auto root = std::move(root_result).value();
    auto stats = runtime.collect_garbage();
    REQUIRE(stats.marked == baseline + depth);
    REQUIRE(stats.collected == 0U);

    root.reset();
    stats = runtime.collect_garbage();
    REQUIRE(stats.collected == depth);
    REQUIRE(runtime.heap_cell_count() == baseline);
}

TEST_CASE("built-in globals and prototype graph are garbage collection roots") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto array_constructor = context.get_global("Array");
    REQUIRE(array_constructor);
    const auto before = runtime.heap_cell_count();
    REQUIRE(before > 0U);

    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.after > 0U);
    REQUIRE(stats.after <= before);

    const auto array_constructor_again = context.get_global("Array");
    REQUIRE(array_constructor_again);
    REQUIRE(array_constructor_again->is_function());

    const auto array = context.array();
    REQUIRE(context.array_push(array, context.number(7.0)));
    const auto popped = context.array_pop(array);
    REQUIRE(popped);
    REQUIRE(popped->is_number());
    REQUIRE(popped->as_number() == 7.0);
}

TEST_CASE("released persistent root can be collected while another root survives") {
    js::Runtime runtime;
    js::Context context(runtime);

    (void)context.object_prototype();
    const auto baseline = runtime.heap_cell_count();
    const auto first = context.object();
    const auto second = context.object();
    auto first_result = runtime.root(first);
    auto second_result = runtime.root(second);
    REQUIRE(first_result);
    REQUIRE(second_result);
    auto first_root = std::move(first_result).value();
    auto second_root = std::move(second_result).value();

    first_root.reset();
    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.collected == 1U);
    REQUIRE(runtime.heap_cell_count() == baseline + 1U);
    REQUIRE(second_root.value().is_object());
}

TEST_CASE("persistent bytecode root keeps heap-backed constants executable across collection") {
    js::Runtime runtime;
    js::Context context(runtime);

    auto chunk = compile(context, "function answer() { return 42; } answer()");
    REQUIRE(chunk);
    auto chunk_root_result = runtime.root_bytecode(*chunk);
    REQUIRE(chunk_root_result);
    auto chunk_root = std::move(chunk_root_result).value();

    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.marked > 0U);

    js::VM vm(context);
    const auto result = vm.run(*chunk);
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 42.0);
}
