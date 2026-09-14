#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {

js::Result<js::Value> eval(js::Context& context, std::string_view source, std::size_t* max_frames = nullptr) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "function test source failed to parse"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    auto result = vm.run(*chunk);
    if (max_frames != nullptr) *max_frames = vm.maximum_frame_depth();
    return result;
}


} // namespace

TEST_CASE("function call maps arguments to parameter local slots") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function add(a, b) { return a + b; } add(2, 3)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("function locals are isolated from caller locals") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let x = 9; function f(x) { let y = x + 1; return y; } f(2) + x");
    REQUIRE(result);
    REQUIRE(result->as_number() == 12.0);
}

TEST_CASE("function declaration is initialized before surrounding statement execution") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let answer = add(4, 5); function add(a, b) { return a + b; } answer");
    REQUIRE(result);
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("nested function calls return through caller frame") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function outer(n) { function twice(x) { return x * 2; } return twice(n) + 1; } outer(6)");
    REQUIRE(result);
    REQUIRE(result->as_number() == 13.0);
}

TEST_CASE("recursive function allocates one frame per active invocation") {
    js::Runtime runtime;
    js::Context context(runtime);
    std::size_t max_frames = 0;
    const auto result = eval(context, "function fact(n) { if (n <= 1) return 1; return n * fact(n - 1); } fact(5)", &max_frames);
    REQUIRE(result);
    REQUIRE(result->as_number() == 120.0);
    REQUIRE(max_frames == 6U); // program frame + fact(5..1)
}

TEST_CASE("return without expression returns undefined") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f() { return; } f()");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("CALL rejects non-function target") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let x = 3; x()");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("accepts extra call arguments") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function one(a) { return a; } one(1, 2)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 1.0);
}

TEST_CASE("initializes missing formal parameters to undefined") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function second(a, b) { return b; } second(1)");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("arguments object retains the actual argument vector") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function inspect(a) { return arguments.length * 100 + arguments[1]; } inspect(4, 7, 9)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 307.0);
}

TEST_CASE("arguments is an ordinary array-like object") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function inspect() { return Array.isArray(arguments); } inspect(1, 2)");
    REQUIRE(result);
    REQUIRE(result->is_boolean());
    REQUIRE(!result->as_boolean());
}

TEST_CASE("parameter named arguments shadows the implicit binding") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function inspect(arguments) { return arguments; } inspect(12, 99)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 12.0);
}

TEST_CASE("exposes function length and name metadata") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function add(a, b) { return 0; } add.length * 10 + (add.name == 'add')");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 21.0);
}

TEST_CASE("native functions safely accept missing arguments") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "Array.isArray()");
    REQUIRE(result);
    REQUIRE(result->is_boolean());
    REQUIRE(!result->as_boolean());
}

TEST_CASE("function can capture an enclosing program binding") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let x = 7; function f() { return x; } f()");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("default parameters initialize left to right") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a=2,b=a+3){return b;} f()");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("later parameter is in TDZ while earlier default initializer runs") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a=b,b=2){return a;} f()");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("rest parameter collects remaining arguments into an Array") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a,...rest){return Array.isArray(rest)*100+rest.length*10+rest[1];} f(1,4,7)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 127.0);
}

TEST_CASE("function length stops before first default and excludes rest") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a,b=2,c,...rest){return 0;} f.length");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 1.0);
}

TEST_CASE("mapped arguments reflects parameter assignment in simple non-strict functions") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a){a=9;return arguments[0];} f(2)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("mapped arguments write reflects into parameter binding") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a){arguments[0]=9;return a;} f(2)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("non-simple parameter list uses unmapped arguments") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a=1){arguments[0]=9;return a;} f(2)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 2.0);
}

TEST_CASE("strict functions use unmapped arguments") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a){'use strict';arguments[0]=9;return a;} f(2)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 2.0);
}

TEST_CASE("duplicate simple non-strict parameters map only the last duplicate") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function f(a,a){a=8;return arguments[0]*10+arguments[1];} f(2,3)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 28.0);
}

TEST_CASE("arrow function captures lexical this") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function outer(){let f=()=>this.x;return f();} let o={x:12,m:outer};o.m()");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 12.0);
}

TEST_CASE("arrow function captures lexical arguments") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function outer(a){let f=()=>arguments[0];return f();} outer(11)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 11.0);
}

TEST_CASE("arrow functions are not constructors") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let F=()=>1; new F()");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("Function prototype call preserves explicit this and arguments") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function add(a,b){return this.x+a+b;} let o={x:10};add.call(o,2,3)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 15.0);
}

TEST_CASE("Function prototype apply reads array-like arguments") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function add(a,b){return this.x+a+b;} let o={x:10};add.apply(o,[2,3])");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 15.0);
}

TEST_CASE("Function prototype bind prepends arguments and binds this") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function add(a,b){return this.x+a+b;} let o={x:10};let f=add.bind(o,2);f(3)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 15.0);
}

TEST_CASE("bound construct forwards construction to target and ignores bound this") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "function C(a,b){this.v=a+b;} let B=C.bind(0,2);let x=new B(5);x.v");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("class methods are callable but not constructible") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "class C { m(){return 1;} } let c=new C(); new c.m()");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::type_error);
}


TEST_CASE("P10.5 function parameters use shared destructuring binding machinery") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "function f([a=2],{x:y=3}){return a*10+y;} f([],{x:4})");
    REQUIRE(result); REQUIRE(result->as_number() == 24.0);
}

TEST_CASE("Test262 bootstrap anonymous function expression executes") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let f = function (x) { return x + 1; }; f(4)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("Test262 bootstrap named function expression has an inner recursive name") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context,
        "let f = function fact(n) { if (n <= 1) return 1; return n * fact(n - 1); }; f(5)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 120.0);
}

TEST_CASE("Test262 bootstrap named function expression name does not leak") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let f = function inner() { return 1; }; typeof inner");
    REQUIRE(result);
    REQUIRE(result->is_string());
    REQUIRE(result->as_string() == "undefined");
}

TEST_CASE("Test262 bootstrap named function expression parameter may shadow inner name") {
    js::Runtime runtime;
    js::Context context(runtime);
    const auto result = eval(context, "let f = function inner(inner) { return inner; }; f(9)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("Test262 bootstrap primitive boolean null and this literals execute") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto truth = eval(context, "true");
    REQUIRE(truth);
    REQUIRE(truth->is_boolean());
    REQUIRE(truth->as_boolean());

    const auto falsity = eval(context, "false");
    REQUIRE(falsity);
    REQUIRE(falsity->is_boolean());
    REQUIRE(!falsity->as_boolean());

    const auto null_value = eval(context, "null");
    REQUIRE(null_value);
    REQUIRE(null_value->is_null());

    const auto this_value = eval(context, "let o = { x: 7, f: function () { return this.x; } }; o.f()");
    REQUIRE(this_value);
    REQUIRE(this_value->is_number());
    REQUIRE(this_value->as_number() == 7.0);
}
