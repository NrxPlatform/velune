#include "test.hpp"

#include <string_view>
#include <js/js.hpp>

namespace {
js::Result<js::Value> eval14(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "test parse failed"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}
}

TEST_CASE("for of iterates arrays through the iterator protocol") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let sum=0; for (let x of [1,2,3,4]) { sum=sum+x; } sum");
    REQUIRE(v); REQUIRE(v->as_number() == 10.0);
}

TEST_CASE("array iterator next returns value and done result objects") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let it=[4,5][Symbol.iterator](); let a=it.next(); let b=it.next(); let c=it.next(); let score=a.value+b.value; if(c.done) score=score+10; score");
    REQUIRE(v); REQUIRE(v->as_number() == 19.0);
}

TEST_CASE("user objects participate in for of by implementing iterator and next") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "function makeIterator(){ let i=0; function next(){ i=i+1; if(i<=3) return {value:i,done:0}; return {value:0,done:1}; } return {next:next}; }"
        "let iterable={}; iterable[Symbol.iterator]=makeIterator; let sum=0; for(let x of iterable){sum=sum+x;} sum");
    REQUIRE(v); REQUIRE(v->as_number() == 6.0);
}

TEST_CASE("for of binding is lexical and does not leak") {
    js::Runtime r; js::Context c(r);
    auto parsed = js::frontend::parse_program("for(let x of [1]){} x"); REQUIRE(parsed);
    auto chunk = js::compiler::compile_program(c, parsed.program());
    REQUIRE(chunk);
    js::VM vm(c); auto result = vm.run(*chunk);
    REQUIRE(result); REQUIRE(result.completion().is_throw());
}

TEST_CASE("for of supports var bindings") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let sum=0; for(var x of [1,2]){sum=sum+x;} sum+x");
    REQUIRE(v); REQUIRE(v->as_number() == 5.0);
}

TEST_CASE("for of supports const bindings while preserving immutability") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let sum=0; for(const x of [1,2]){sum=sum+x;} sum");
    REQUIRE(v); REQUIRE(v->as_number() == 3.0);
    auto immutable = eval14(c, "for(const x of [1]){x=2;} 0");
    REQUIRE(!immutable); REQUIRE(immutable.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("for of reports non iterable values through ordinary method call failure") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "for(let x of {value:1}){} 0");
    REQUIRE(!v); REQUIRE(v.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("for of requires iterator next to return an object result") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "function makeIterator(){ function next(){ return 1; } return {next:next}; }"
        "let iterable={}; iterable[Symbol.iterator]=makeIterator; for(let x of iterable){} 0");
    REQUIRE(!v); REQUIRE(v.error().code() == js::ErrorCode::uncaught_exception);
}

TEST_CASE("of remains a contextual keyword outside for of grammar") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let of=3; let o={of:4}; of+o.of");
    REQUIRE(v); REQUIRE(v->as_number() == 7.0);
}

TEST_CASE("P10.4 classic for supports lexical initializer test update break and continue") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "let sum=0; for(let i=0;i<6;i=i+1){ if(i===2) continue; if(i===5) break; sum=sum+i; } sum");
    REQUIRE(v); REQUIRE(v->as_number() == 8.0);
}

TEST_CASE("P10.4 do while executes body before condition") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let n=0; do { n=n+1; } while(0); n");
    REQUIRE(v); REQUIRE(v->as_number() == 1.0);
}

TEST_CASE("P10.4 generalized for of supports var const and assignment lhs") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "let sum=0; var x=0; for(var y of [1,2]){sum=sum+y;} for(const z of [3,4]){sum=sum+z;} for(x of [5]){sum=sum+x;} sum+y+x");
    REQUIRE(v); REQUIRE(v->as_number() == 22.0);
}

TEST_CASE("P10.4 for in visits own and inherited enumerable string keys") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "function P(){} P.prototype.p=1; let o=new P(); o.a=2; let hits=0;"
        "for(let k in o){if(k===\"a\"||k===\"p\") hits=hits+1;} hits");
    REQUIRE(v); REQUIRE(v->as_number() == 2.0);
}

TEST_CASE("P10.4 switch selects case falls through and supports default") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "let x=0; switch(2){case 1:x=10;break;case 2:x=2;default:x=x+3;} x");
    REQUIRE(v); REQUIRE(v->as_number() == 5.0);
}

TEST_CASE("P10.4 labelled break and continue resolve structured targets") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "let x=0; outer: for(let i=0;i<3;i=i+1){for(let j=0;j<3;j=j+1){"
        "if(j===1) continue outer; x=x+1;}} done: {x=x+10; break done; x=999;} x");
    REQUIRE(v); REQUIRE(v->as_number() == 13.0);
}

TEST_CASE("P10.4 debugger statement is semantically harmless") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let x=1; debugger; x=x+1; x");
    REQUIRE(v); REQUIRE(v->as_number() == 2.0);
}

TEST_CASE("P10.4 switch owns one lexical scope across clauses") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let out=0; switch(1){case 1: let x=4; out=x; break; default: out=9;} out");
    REQUIRE(v); REQUIRE(v->as_number() == 4.0);
}

TEST_CASE("P10.4 var declarations hoist through new control flow nodes") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "while(0){var a=1;} switch(0){case 1: var b=2;} typeof a+\":\"+typeof b");
    REQUIRE(v); REQUIRE(v->as_string() == "undefined:undefined");
}


TEST_CASE("P10.5 for-of accepts destructuring binding patterns") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c, "let out=0; for(const [x,y] of [[1,2],[3,4]]){out=out+x*10+y;} out");
    REQUIRE(v); REQUIRE(v->as_number() == 46.0);
}

TEST_CASE("P10.5 classic for creates distinct captured binding identities per iteration") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "let f0;let f1;let f2;for(let i=0;i<3;i=i+1){"
        "if(i===0)f0=()=>i;if(i===1)f1=()=>i;if(i===2)f2=()=>i;}f0()*100+f1()*10+f2()");
    REQUIRE(v); REQUIRE(v->as_number() == 12.0);
}

TEST_CASE("P10.5 for-of creates distinct captured lexical bindings per iteration") {
    js::Runtime r; js::Context c(r);
    auto v = eval14(c,
        "let f0;let f1;for(const x of [7,8]){if(x===7)f0=()=>x;else f1=()=>x;}f0()*10+f1()");
    REQUIRE(v); REQUIRE(v->as_number() == 78.0);
}
