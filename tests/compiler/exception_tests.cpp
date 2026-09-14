#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {

js::Result<js::Value> eval11(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "test source failed to parse"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}

js::ExecutionResult eval_execution(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::EngineFailure{js::Error{js::ErrorCode::internal, "test source failed to parse"}};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return js::EngineFailure{chunk.error()};
    js::VM vm(context);
    return vm.run(*chunk);
}

js::ExecutionResult native_throw(js::Context& context, js::Value, std::span<const js::Value>) {
    return js::Completion::throw_(context.number(7));
}

js::ExecutionResult native_engine_failure(js::Context&, js::Value, std::span<const js::Value>) {
    return js::EngineFailure{js::EngineFailureCode::InternalInvariant, "synthetic engine failure"};
}

js::ExecutionResult native_reenter(js::Context& context, js::Value, std::span<const js::Value> arguments) {
    if (arguments.empty()) return js::EngineFailure{js::EngineFailureCode::HostContractViolation, "reenter requires a callable"};
    return context.invoke(arguments[0], {});
}

} // namespace

TEST_CASE("throw transfers control to catch binding") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let x = 0; try { throw 7; } catch (e) { x = e; } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("exception unwinds through nested call frames") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "function inner() { throw 3; } function outer() { inner(); return 99; } let x = 0; try { outer(); } catch (e) { x = e + 4; } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("catch parameter is a lexical binding that shadows outer name") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let e = 2; let x = 0; try { throw 7; } catch (e) { x = e; } e + x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("finally runs on normal completion") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let x = 0; try { x = 1; } finally { x = x + 2; } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 3.0);
}

TEST_CASE("return is suspended while finally executes") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let x = 0; function f() { try { return 4; } finally { x = 9; } } f() + x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 13.0);
}

TEST_CASE("return inside finally overrides pending return") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "function f() { try { return 1; } finally { return 9; } } f()");
    REQUIRE(result);
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("throw inside finally overrides pending exception") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let x = 0; try { try { throw 1; } finally { throw 2; } } catch (e) { x = e; } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 2.0);
}

TEST_CASE("catch normal completion still executes finally") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let x = 0; try { throw 4; } catch (e) { x = e; } finally { x = x + 1; } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("try finally without catch rethrows after cleanup") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let x = 0; try { try { throw 6; } finally { x = 1; } } catch (e) { x = x + e; } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("captured locals are closed when exception unwinding destroys frame") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context,
        "let saved; "
        "function outer() { let x = 41; function read() { return x + 1; } saved = read; throw 5; } "
        "try { outer(); } catch (e) { let ignored = e; } "
        "saved()");
    REQUIRE(result);
    REQUIRE(result->as_number() == 42.0);
}

TEST_CASE("var declarations inside try participate in function var scope") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "try { var x = 5; } finally { } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("uncaught JavaScript exception remains distinct from native engine error") {
    js::Runtime runtime;
    js::Context context(runtime);
    auto parsed = js::frontend::parse_program("throw 33;");
    REQUIRE(parsed);
    auto chunk = js::compiler::compile_program(context, parsed.program());
    REQUIRE(chunk);
    js::VM vm(context);
    const auto result = vm.run(*chunk);
    REQUIRE(result);
    REQUIRE(result.completion().is_throw());
    REQUIRE(result.completion().value().is_number());
    REQUIRE(result.completion().value().as_number() == 33.0);
}


TEST_CASE("native engine errors are not mistaken for JavaScript thrown values") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let caught = 0; try { Object.getPrototypeOf(1); } catch (e) { caught = 1; } caught");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("same VM semantic invocation can re-enter JavaScript normally") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto inner_result = eval_execution(context, "function inner() { return 42; } inner");
    REQUIRE(inner_result);
    REQUIRE(inner_result.completion().is_normal());
    const js::Value inner = inner_result.completion().value();
    const js::Value reenter = context.native_function("reenter", 1, native_reenter);
    const js::Value argument[] = {inner};
    js::VM vm(context);
    const auto result = vm.invoke(reenter, js::Value::undefined(), argument);
    REQUIRE(result);
    REQUIRE(result.completion().is_normal());
    REQUIRE(result.completion().value().as_number() == 42.0);
}

TEST_CASE("re-entered JavaScript throw returns through native code and is caught by caller") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto inner_result = eval_execution(context, "function inner() { throw 7; } inner");
    REQUIRE(inner_result && inner_result.completion().is_normal());
    const auto wrapper_result = eval_execution(context, "function wrapper(f, g) { try { return f(g); } catch (e) { return e + 1; } } wrapper");
    REQUIRE(wrapper_result && wrapper_result.completion().is_normal());
    const js::Value reenter = context.native_function("reenter", 1, native_reenter);
    const js::Value arguments[] = {reenter, inner_result.completion().value()};
    js::VM vm(context);
    const auto result = vm.invoke(wrapper_result.completion().value(), js::Value::undefined(), arguments);
    REQUIRE(result);
    REQUIRE(result.completion().is_normal());
    REQUIRE(result.completion().value().as_number() == 8.0);
}

TEST_CASE("native JavaScript throw is catchable as a Completion Throw") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto wrapper_result = eval_execution(context, "function wrapper(f) { try { return f(); } catch (e) { return e + 2; } } wrapper");
    REQUIRE(wrapper_result && wrapper_result.completion().is_normal());
    const js::Value throwing_native = context.native_function("throwingNative", 0, native_throw);
    const js::Value arguments[] = {throwing_native};
    js::VM vm(context);
    const auto result = vm.invoke(wrapper_result.completion().value(), js::Value::undefined(), arguments);
    REQUIRE(result);
    REQUIRE(result.completion().is_normal());
    REQUIRE(result.completion().value().as_number() == 9.0);
}

TEST_CASE("engine failure bypasses JavaScript catch") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto wrapper_result = eval_execution(context, "function wrapper(f) { try { return f(); } catch (e) { return 99; } } wrapper");
    REQUIRE(wrapper_result && wrapper_result.completion().is_normal());
    const js::Value failing_native = context.native_function("failingNative", 0, native_engine_failure);
    const js::Value arguments[] = {failing_native};
    js::VM vm(context);
    const auto result = vm.invoke(wrapper_result.completion().value(), js::Value::undefined(), arguments);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::EngineFailureCode::InternalInvariant);
}

TEST_CASE("break completion crosses finally before reaching loop target") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let x = 0; while (1 < 2) { try { break; } finally { x = 7; } } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("continue completion crosses finally before reaching loop target") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "let i = 0; let x = 0; while (i < 2) { i = i + 1; try { continue; } finally { x = x + 3; } } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 6.0);
}

TEST_CASE("abrupt completion in finally overrides pending break") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval11(context, "function f() { while (1 < 2) { try { break; } finally { return 9; } } return 1; } f()");
    REQUIRE(result);
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("throw line terminator is rejected by frontend") {
    const auto parsed = js::frontend::parse_program("throw\n1;");
    REQUIRE(!parsed);
}

TEST_CASE("verifier rejects invalid exception handler targets") {
    js::bytecode::BytecodeBuilder builder;
    builder.emit(js::bytecode::OpCode::undefined);
    builder.emit(js::bytecode::OpCode::return_);
    builder.add_exception_handler(js::bytecode::ExceptionHandler{0U, 1U, 99U, 100U, js::bytecode::no_handler_target, js::bytecode::no_handler_target});
    auto chunk = std::move(builder).finish();
    js::bytecode::BytecodeVerifier verifier;
    const auto verified = verifier.verify(chunk);
    REQUIRE(!verified);
    REQUIRE(verified.error().code() == js::ErrorCode::bytecode_error);
}


TEST_CASE("P10.5 catch bindings accept object patterns") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval11(context,
        "let out=0; try { throw {message:6}; } catch ({message}) { out=message; } out");
    REQUIRE(result); REQUIRE(result->as_number() == 6.0);
}
