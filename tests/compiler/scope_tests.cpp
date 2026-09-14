#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {

js::Result<js::Value> eval(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) {
        return js::Error{js::ErrorCode::internal, "scope test source failed to parse"};
    }

    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) {
        return chunk.error();
    }

    js::VM vm(context);
    return vm.run(*chunk);
}

js::Result<js::bytecode::BytecodeChunk> compile(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) {
        return js::Error{js::ErrorCode::internal, "scope test source failed to parse"};
    }
    return js::compiler::compile_program(context, parsed.program());
}

} // namespace

TEST_CASE("block lexical binding shadows outer binding") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 1; { let x = 2; x = x + 3; } x");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 1.0);
}

TEST_CASE("nested lexical binding can read outer binding") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 2; { let y = x + 3; } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 2.0);
}

TEST_CASE("block lexical binding is not visible after block and fails at evaluation") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto chunk = compile(context, "{ let hidden = 3; } hidden");
    REQUIRE(chunk);
    const auto result = eval(context, "{ let hidden = 3; } hidden");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("Infinity is resolved through the runtime global environment") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto chunk = compile(context, "Infinity");
    REQUIRE(chunk);
    const auto result = eval(context, "Infinity");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() > 0.0);
    REQUIRE(result->as_number() == result->as_number() * 2.0);
}

TEST_CASE("arbitrary unresolved identifier compiles and throws ReferenceError only at evaluation") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto chunk = compile(context, "definitelyNotDeclaredAnywhere");
    REQUIRE(chunk);
    const auto result = eval(context, "definitelyNotDeclaredAnywhere");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("lexical shadow exists before declaration and triggers TDZ guard") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto chunk = compile(context, "let x = 1; { x; let x = 2; }");
    REQUIRE(chunk);
    const auto result = eval(context, "let x = 1; { x; let x = 2; }");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("initializer cannot read its own uninitialized let binding") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto chunk = compile(context, "let x = x;");
    REQUIRE(chunk);
    const auto result = eval(context, "let x = x;");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("earlier declarator initializes before later declarator") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 2, y = x + 3; y");
    REQUIRE(result);
    REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("const binding can be read after initialization") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "const x = 4; x + 1");
    REQUIRE(result);
    REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("assignment to const binding is rejected") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto chunk = compile(context, "const x = 1; x = 2;");
    REQUIRE(chunk);
    const auto result = eval(context, "const x = 1; x = 2;");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::type_error);
}


TEST_CASE("closure observes runtime TDZ state") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "function read(){ return x; } read(); let x = 1;");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("duplicate lexical declaration in one scope is rejected") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto parsed = js::frontend::parse_program("let x = 1; const x = 2;");
    REQUIRE(!parsed);
    REQUIRE(parsed.diagnostic().message.find("duplicate lexical declaration") != std::string::npos);
}

TEST_CASE("var binding is hoisted and initially undefined") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "x; var x = 2;");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("repeated var declaration shares one binding") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "var x = 7; var x; x");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("var declared in block resolves in program var scope") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "{ var x = 6; } x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 6.0);
}

TEST_CASE("var conflicts with lexical binding in same containing statement list") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto parsed = js::frontend::parse_program("{ let x = 1; { var x = 2; } }");
    REQUIRE(!parsed);
    REQUIRE(parsed.diagnostic().message.find("var declaration conflicts with lexical declaration") != std::string::npos);
}

TEST_CASE("shadowed bindings receive distinct local slots") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto chunk = compile(context, "let x = 1; { let x = 2; }");
    REQUIRE(chunk);
    REQUIRE(chunk->local_count() == 2U);
}
