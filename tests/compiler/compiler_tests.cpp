#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {

js::Result<js::Value> eval(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) {
        return js::Error{js::ErrorCode::internal, "test source failed to parse"};
    }

    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) {
        return chunk.error();
    }

    js::VM vm(context);
    return vm.run(*chunk);
}

} // namespace

TEST_CASE("compiler executes arithmetic precedence from AST") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "2 * 3 + 1");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("compiler preserves non-commutative AST order") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "10 - 2 * 3");
    REQUIRE(result);
    REQUIRE(result->as_number() == 4.0);
}

TEST_CASE("compiler assigns top-level local slots") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 2; x * 4 + 1");
    REQUIRE(result);
    REQUIRE(result->as_number() == 9.0);
}

TEST_CASE("assignment expression updates a top-level local") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 2; x = x * 5; x + 1");
    REQUIRE(result);
    REQUIRE(result->as_number() == 11.0);
}

TEST_CASE("declaration without initializer stores undefined") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x; x");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("program without final expression returns undefined") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 2;");
    REQUIRE(result);
    REQUIRE(result->is_undefined());
}

TEST_CASE("compiler preserves unresolved identifiers for runtime resolution") {
    js::Runtime runtime;
    js::Context context(runtime);

    auto parsed = js::frontend::parse_program("missing + 1");
    REQUIRE(parsed);
    const auto chunk = js::compiler::compile_program(context, parsed.program());
    REQUIRE(chunk);
    js::VM vm(context);
    const auto result = vm.run(*chunk);
    REQUIRE(!result);
    REQUIRE(result.error().legacy_error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("compiler supports if control flow") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 1; if (x <= 1) { x = 4; } x");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 4.0);
}

TEST_CASE("addition performs primitive string concatenation") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto left_string = eval(context, "'answer=' + 42");
    REQUIRE(left_string);
    REQUIRE(left_string->is_string());
    REQUIRE(left_string->as_string() == "answer=42");

    const auto right_string = eval(context, "1 + '2'");
    REQUIRE(right_string);
    REQUIRE(right_string->is_string());
    REQUIRE(right_string->as_string() == "12");
}

TEST_CASE("numeric operators apply ToNumber to strings") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "'12' / '3' + ('7' % 4)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 7.0);
}

TEST_CASE("comparison operators share abstract comparison semantics") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto relational = eval(context, "'10' < 2");
    REQUIRE(relational && relational->is_boolean() && !relational->as_boolean());

    const auto lexical = eval(context, "'10' < '2'");
    REQUIRE(lexical && lexical->is_boolean() && lexical->as_boolean());

    const auto greater_equal = eval(context, "3 >= '3'");
    REQUIRE(greater_equal && greater_equal->is_boolean() && greater_equal->as_boolean());
}

TEST_CASE("supports loose and strict equality operators") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto loose = eval(context, "1 == '1'");
    REQUIRE(loose && loose->is_boolean() && loose->as_boolean());

    const auto strict = eval(context, "1 === '1'");
    REQUIRE(strict && strict->is_boolean() && !strict->as_boolean());

    const auto not_equal = eval(context, "1 != '1'");
    REQUIRE(not_equal && not_equal->is_boolean() && !not_equal->as_boolean());
}

TEST_CASE("unary plus minus and not use centralized conversion") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto positive = eval(context, "+'5'");
    REQUIRE(positive && positive->is_number() && positive->as_number() == 5.0);

    const auto negative = eval(context, "-'5'");
    REQUIRE(negative && negative->is_number() && negative->as_number() == -5.0);

    const auto logical_not = eval(context, "!'non-empty'");
    REQUIRE(logical_not && logical_not->is_boolean() && !logical_not->as_boolean());
}

TEST_CASE("branches use the same ToBoolean operation") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto empty = eval(context, "let x=1; if ('') { x=2; } x");
    REQUIRE(empty && empty->as_number() == 1.0);

    const auto nonempty = eval(context, "let x=1; if ('x') { x=2; } x");
    REQUIRE(nonempty && nonempty->as_number() == 2.0);
}

TEST_CASE("logical operators short-circuit and preserve operand values") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto and_result = eval(context, "let touched=0; let v=0 && (touched=1); touched*10+v");
    REQUIRE(and_result && and_result->is_number() && and_result->as_number() == 0.0);

    const auto or_result = eval(context, "let touched=0; let v='left' || (touched=1); touched");
    REQUIRE(or_result && or_result->is_number() && or_result->as_number() == 0.0);

    const auto values = eval(context, "('' || 'fallback') + ':' + ('ok' && 'right')");
    REQUIRE(values && values->is_string() && values->as_string() == "fallback:right");
}

TEST_CASE("bitwise and shift operators use 32-bit conversion semantics") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "('5' & 3) + (8 >> 2) + (-1 >>> 31)");
    REQUIRE(result && result->is_number() && result->as_number() == 4.0);

    const auto complement = eval(context, "~0");
    REQUIRE(complement && complement->is_number() && complement->as_number() == -1.0);

    const auto signed_shift = eval(context, "1 << 31");
    REQUIRE(signed_shift && signed_shift->is_number() && signed_shift->as_number() == -2147483648.0);
}

TEST_CASE("while break and continue compile as structured jumps") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context,
        "let i=0; let sum=0; "
        "while(i<6){ i++; if(i==2){continue;} if(i==5){break;} sum=sum+i; } "
        "sum");
    REQUIRE(result && result->is_number() && result->as_number() == 8.0);
}

TEST_CASE("break and continue also target for-of loops") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context,
        "let sum=0; for(let x of [1,2,3,4]){ if(x==2){continue;} if(x==4){break;} sum=sum+x; } sum");
    REQUIRE(result && result->is_number() && result->as_number() == 4.0);
}

TEST_CASE("prefix and postfix update preserve JavaScript result values") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto identifier = eval(context, "let x='4'; let old=x++; let now=++x; old*100+now*10+x");
    REQUIRE(identifier && identifier->is_number() && identifier->as_number() == 466.0);

    const auto member = eval(context, "let o={x:'4'}; let old=o.x++; let now=++o.x; old*100+now*10+o.x");
    REQUIRE(member && member->is_number() && member->as_number() == 466.0);

    const auto computed = eval(context, "let a=[5]; let old=a[0]--; old*10+a[0]");
    REQUIRE(computed && computed->is_number() && computed->as_number() == 54.0);
}

TEST_CASE("loop jumps crossing finally lower to abrupt completion bytecode") {
    js::Runtime runtime;
    js::Context context(runtime);
    auto parsed = js::frontend::parse_program("while(1){ try { break; } finally { 0; } }");
    REQUIRE(parsed);
    const auto chunk = js::compiler::compile_program(context, parsed.program());
    REQUIRE(chunk);
    js::bytecode::BytecodeVerifier verifier;
    REQUIRE(verifier.verify(*chunk));
}


TEST_CASE("computed update reference evaluates base and key exactly once") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context,
        "let baseCount=0; let keyCount=0; "
        "let o={x:4}; "
        "function base(){baseCount++; return o;} "
        "function key(){keyCount++; return 'x';} "
        "let old=base()[key()]++; "
        "baseCount*100 + keyCount*10 + old + o.x");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 119.0);
}

TEST_CASE("computed assignment evaluates reference before right hand side") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context,
        "let order=''; let o={x:0}; "
        "function key(){order=order+'k'; return 'x';} "
        "function rhs(){order=order+'r'; return 9;} "
        "o[key()] = rhs(); order + ':' + o.x");
    REQUIRE(result);
    REQUIRE(result->is_string());
    REQUIRE(result->as_string() == "kr:9");
}

TEST_CASE("P10.2 conditional expression evaluates exactly one branch") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 0; let y = 1 ? (x = x + 1) : (x = x + 10); x * 10 + y");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 11.0);
}

TEST_CASE("P10.2 sequence expression evaluates left to right and returns final value") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 0; (x = 1, x = x + 2, x * 4)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 12.0);
}

TEST_CASE("P10.2 exponentiation is right associative") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "2 ** 3 ** 2");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 512.0);
}

TEST_CASE("P10.2 typeof distinguishes unresolved identifiers from TDZ bindings") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto missing = eval(context, "typeof totallyMissing");
    REQUIRE(missing);
    REQUIRE(missing->is_string());
    REQUIRE(missing->as_string() == "undefined");

    const auto tdz = eval(context, "typeof x; let x = 1;");
    REQUIRE(!tdz);
    REQUIRE(tdz.error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("P10.2 typeof reports core runtime categories") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto function_type = eval(context, "function f(){} typeof f");
    REQUIRE(function_type && function_type->is_string() && function_type->as_string() == "function");

}

TEST_CASE("P10.2 void preserves side effects and produces undefined") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let x = 0; let y = void (x = 7); x + (typeof y === 'undefined' ? 1 : 100)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 8.0);
}

TEST_CASE("P10.2 delete uses property reference semantics") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "let n = 0; let o = {x: 1}; function key(){ n = n + 1; return 'x'; } let ok = delete o[key()]; n * 100 + (ok ? 10 : 0) + (typeof o.x === 'undefined' ? 1 : 0)");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 111.0);
}

TEST_CASE("P10.2 delete of declared identifier is false and strict delete is rejected") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto sloppy = eval(context, "let x = 1; delete x");
    REQUIRE(sloppy && sloppy->is_boolean() && !sloppy->as_boolean());

    auto parsed = js::frontend::parse_program("'use strict'; let x = 1; delete x");
    REQUIRE(!parsed);
    REQUIRE(parsed.diagnostic().message.find("delete of an unqualified identifier") != std::string::npos);
}

TEST_CASE("P10.2 in checks own and inherited properties") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto own = eval(context, "'x' in {x: 1}");
    REQUIRE(own && own->is_boolean() && own->as_boolean());

    const auto inherited = eval(context, "function F(){} F.prototype.x = 1; let o = new F(); 'x' in o");
    REQUIRE(inherited && inherited->is_boolean() && inherited->as_boolean());
}

TEST_CASE("P10.2 in rejects primitive right hand side") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "'x' in 1");
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("P10.2 instanceof follows prototype chains and bound targets") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto direct = eval(context, "function F(){} let o = new F(); o instanceof F");
    REQUIRE(direct && direct->is_boolean() && direct->as_boolean());

    const auto bound = eval(context, "function F(){} let B = F.bind(0); let o = new F(); o instanceof B");
    REQUIRE(bound && bound->is_boolean() && bound->as_boolean());
}

TEST_CASE("P10.2 instanceof honors Symbol.hasInstance") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context, "function F(){} function yes(x){ return 1; } F[Symbol.hasInstance] = yes; 1 instanceof F");
    REQUIRE(result && result->is_boolean() && result->as_boolean());
}

TEST_CASE("P10.3 arithmetic and bitwise compound assignments reuse ordinary operator semantics") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto arithmetic = eval(context,
        "let a=5; a+=3; a*=2; a-=4; a/=3; a%=5; a**=2; a");
    REQUIRE(arithmetic);
    REQUIRE(arithmetic->is_number());
    REQUIRE(arithmetic->as_number() == 16.0);

    const auto bitwise = eval(context,
        "let x=5; x<<=2; x>>=1; x>>>=1; x|=8; x&=13; x^=3; x");
    REQUIRE(bitwise);
    REQUIRE(bitwise->is_number());
    REQUIRE(bitwise->as_number() == 14.0);

    const auto concatenate = eval(context, "let s='a'; s += 2; s");
    REQUIRE(concatenate);
    REQUIRE(concatenate->is_string());
    REQUIRE(concatenate->as_string() == "a2");
}

TEST_CASE("P10.3 compound assignment evaluates computed reference exactly once") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context,
        "let baseCount=0; let keyCount=0; let rhsCount=0; let o={x:4}; "
        "function base(){baseCount++; return o;} "
        "function key(){keyCount++; return 'x';} "
        "function rhs(){rhsCount++; return 3;} "
        "let value=(base()[key()] += rhs()); "
        "baseCount*1000 + keyCount*100 + rhsCount*10 + value + o.x");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 1124.0);
}

TEST_CASE("P10.3 logical AND assignment short circuits RHS and returns original value") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto skipped = eval(context,
        "let x=0; let calls=0; function rhs(){calls++; return 7;} "
        "let value=(x &&= rhs()); calls*100 + x*10 + value");
    REQUIRE(skipped);
    REQUIRE(skipped->is_number());
    REQUIRE(skipped->as_number() == 0.0);

    const auto assigned = eval(context,
        "let x=2; let calls=0; function rhs(){calls++; return 7;} "
        "let value=(x &&= rhs()); calls*100 + x*10 + value");
    REQUIRE(assigned);
    REQUIRE(assigned->is_number());
    REQUIRE(assigned->as_number() == 177.0);
}

TEST_CASE("P10.3 logical OR assignment short circuits RHS and returns correct value") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto skipped = eval(context,
        "let x=2; let calls=0; function rhs(){calls++; return 7;} "
        "let value=(x ||= rhs()); calls*100 + x*10 + value");
    REQUIRE(skipped);
    REQUIRE(skipped->is_number());
    REQUIRE(skipped->as_number() == 22.0);

    const auto assigned = eval(context,
        "let x=0; let calls=0; function rhs(){calls++; return 7;} "
        "let value=(x ||= rhs()); calls*100 + x*10 + value");
    REQUIRE(assigned);
    REQUIRE(assigned->is_number());
    REQUIRE(assigned->as_number() == 177.0);
}

TEST_CASE("P10.3 nullish assignment distinguishes undefined from falsy values") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto undefined_case = eval(context,
        "let x; let calls=0; function rhs(){calls++; return 5;} "
        "let value=(x ?" "?= rhs()); calls*100 + x*10 + value");
    REQUIRE(undefined_case);
    REQUIRE(undefined_case->is_number());
    REQUIRE(undefined_case->as_number() == 155.0);

    const auto zero_case = eval(context,
        "let x=0; let calls=0; function rhs(){calls++; return 5;} "
        "let value=(x ?" "?= rhs()); calls*100 + x*10 + value");
    REQUIRE(zero_case);
    REQUIRE(zero_case->is_number());
    REQUIRE(zero_case->as_number() == 0.0);
}

TEST_CASE("P10.3 logical assignment evaluates computed reference once and conditionally evaluates RHS") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context,
        "let baseCount=0; let keyCount=0; let rhsCount=0; let o={x:0}; "
        "function base(){baseCount++; return o;} "
        "function key(){keyCount++; return 'x';} "
        "function rhs(){rhsCount++; return 9;} "
        "let value=(base()[key()] ||= rhs()); "
        "baseCount*1000 + keyCount*100 + rhsCount*10 + value + o.x");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 1128.0);
}

TEST_CASE("P10.3 abrupt RHS completion performs no compound assignment write") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context,
        "let o={x:4}; function boom(){throw 9;} "
        "try { o.x += boom(); } catch (e) { 0; } o.x");
    REQUIRE(result);
    REQUIRE(result->is_number());
    REQUIRE(result->as_number() == 4.0);
}


TEST_CASE("P10.5 array destructuring supports elision defaults rest and nesting") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "let [a,,b=5,...rest]=[1,2,{}.missing,7,8]; let [[n]]=[[3]]; a*1000+b*100+rest[1]*10+n");
    REQUIRE(result); REQUIRE(result->as_number() == 1583.0);
}

TEST_CASE("P10.5 object destructuring supports rename defaults computed keys and rest") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "let key='x'; let { [key]: y=4, z, ...rest }={z:2,q:9}; y*100+z*10+rest.q");
    REQUIRE(result); REQUIRE(result->as_number() == 429.0);
}

TEST_CASE("P10.5 destructuring assignment writes through ordinary References") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "let a=0; let o={v:0}; [a,o.v]=[3,7]; a*10+o.v");
    REQUIRE(result); REQUIRE(result->as_number() == 37.0);
}

TEST_CASE("P10.6 array literals preserve holes and append iterable spread") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "let a=[1,,...[2,3],4]; a.length*100 + (1 in a)*10 + a[2]");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 502.0);
}

TEST_CASE("P10.6 object literals support shorthand computed names and spread") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "let x=7; let k='q'; let o={x,[k]:9,...{z:11}}; o.x*100+o.q*10+o.z");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 801.0);
}

TEST_CASE("P10.6 object spread copies enumerable data and ignores undefined source") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "let src={x:4}; let u={}.missing; let o={...u,...src}; o.x");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 4.0);
}

TEST_CASE("P10.6 argument spread preserves method receiver") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "function f(a,b){return this.x+a+b;} let o={x:10,f}; o.f(...[1,2])");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 13.0);
}

TEST_CASE("P10.6 constructor spread expands iterable arguments") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "function C(a,b){this.x=a+b;} new C(...[2,3]).x");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 5.0);
}

TEST_CASE("P10.6 untagged templates evaluate substitutions in order") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "let x=7; let y=9; `A${x}B${y}C`");
    REQUIRE(result); REQUIRE(result->is_string()); REQUIRE(result->as_string() == "A7B9C");
}

TEST_CASE("P10.6 tagged templates expose cooked and raw strings") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "function tag(s){return s[0].length+':'+s.raw[0].length;} tag`a\\nb`");
    REQUIRE(result); REQUIRE(result->is_string()); REQUIRE(result->as_string() == "3:4");
}

TEST_CASE("P10.6 tagged template receives substitutions") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "function tag(s,x){return s[0]+':'+s.raw[0]+':'+x;} let x=4; tag`hi${x}`");
    REQUIRE(result); REQUIRE(result->is_string()); REQUIRE(result->as_string() == "hi:hi:4");
}

TEST_CASE("P10.6 tagged template object identity is stable per site") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "function tag(s){return s;} function f(){return tag`x`;} let a=f(); a===f()");
    REQUIRE(result); REQUIRE(result->is_boolean()); REQUIRE(result->as_boolean());
}

TEST_CASE("P10.6 tagged member template preserves tag receiver") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context, "function tag(s,v){return this.x+v;} let o={x:5,tag}; o.tag`${2}`");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 7.0);
}


TEST_CASE("P10.7 nullish coalescing uses nullish rather than truthiness") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "let u={}.missing; let calls=0; function rhs(){calls++; return 9;} "
        "let a=(u ?? rhs()); let b=(0 ?? rhs()); calls*100+a*10+b");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 190.0);
}

TEST_CASE("P10.7 optional property short-circuits computed key evaluation") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "let u={}.missing; let calls=0; function key(){calls++; return 'x';} "
        "let v=u?.[key()]; calls*10 + (typeof v === 'undefined')");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 1.0);
}

TEST_CASE("P10.7 optional member call preserves receiver") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "function f(){return this.x;} let o={x:7,f}; o?.f() + o.f?.() + o?.f?.()");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 21.0);
}

TEST_CASE("P10.7 optional call evaluates member base once and suppresses arguments when nullish") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "let bases=0; let args=0; let o={}; function base(){bases++; return o;} "
        "function arg(){args++; return 1;} base().f?.(arg()); bases*10+args");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 10.0);
}

TEST_CASE("P10.7 optional chain short-circuits the remaining chain") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "let u={}.missing; let v=u?.x.y(); typeof v");
    REQUIRE(result); REQUIRE(result->is_string()); REQUIRE(result->as_string() == "undefined");
}

TEST_CASE("P10.7 optional call supports spread while preserving receiver") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "function f(a,b){return this.x+a+b;} let o={x:10,f}; o.f?.(...[2,3])");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 15.0);
}


TEST_CASE("P10.7 delete optional chain deletes property and short-circuits to true") {
    js::Runtime runtime; js::Context context(runtime);
    const auto result = eval(context,
        "let o={x:1}; let a=delete o?.x; let u={}.missing; let b=delete u?.x; "
        "a*100 + b*10 + ('x' in o)");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 110.0);
}
