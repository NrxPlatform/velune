#include "test.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

#include <js/js.hpp>

namespace {
js::module::ModuleLoader map_loader(std::unordered_map<std::string, std::string> sources) {
    return [sources = std::move(sources)](std::string_view specifier, std::string_view) -> js::Result<js::module::ModuleSource> {
        const auto found = sources.find(std::string(specifier));
        if (found == sources.end()) {
            return js::Error{js::ErrorCode::reference_error, "module not found: " + std::string(specifier)};
        }
        return js::module::ModuleSource{found->first, found->second};
    };
}
}

TEST_CASE("modules link dependencies and evaluate exported declarations") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"math", "export const base=40; export function add2(){return base+2;}"},
        {"main", "import {add2} from \"math\"; export let result=add2();"},
    }));
    auto evaluated = modules.evaluate("main");
    REQUIRE(evaluated);
    auto result = modules.get_export("main", "result");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 42.0);
    REQUIRE(modules.module_count() == 2U);
}

TEST_CASE("module imports are live bindings rather than snapshots") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"counter", "export let count=0; export function inc(){count=count+1; return count;}"},
        {"main", "import {count,inc} from \"counter\"; inc(); inc(); export let observed=count;"},
    }));
    auto evaluated = modules.evaluate("main"); REQUIRE(evaluated);
    auto observed = modules.get_export("main", "observed");
    auto count = modules.get_export("counter", "count");
    REQUIRE(observed); REQUIRE(count);
    REQUIRE(observed->as_number() == 2.0);
    REQUIRE(count->as_number() == 2.0);
}

TEST_CASE("nested functions can read imported live bindings") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"dep", "export let n=7; export function bump(){n=n+1;}"},
        {"main", "import {n,bump} from \"dep\"; function read(){return n;} bump(); export let result=read();"},
    }));
    auto evaluated = modules.evaluate("main"); REQUIRE(evaluated);
    auto result = modules.get_export("main", "result"); REQUIRE(result);
    REQUIRE(result->as_number() == 8.0);
}

TEST_CASE("export lists alias local live bindings") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"entry", "let x=3; export {x}; x=x+4;"},
    }));
    auto evaluated = modules.evaluate("entry"); REQUIRE(evaluated);
    auto x = modules.get_export("entry", "x"); REQUIRE(x);
    REQUIRE(x->as_number() == 7.0);
}

TEST_CASE("module linker rejects missing exports before evaluation") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"dep", "export let x=1;"},
        {"main", "import {missing} from \"dep\"; export let result=missing;"},
    }));
    auto linked = modules.link("main");
    REQUIRE(!linked); REQUIRE(linked.error().code() == js::ErrorCode::compile_error);
}

TEST_CASE("module import assignment fails at runtime") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"dep", "export let x=1;"},
        {"main", "import {x} from \"dep\"; x=2; export let result=x;"},
    }));
    auto linked = modules.link("main");
    REQUIRE(linked);
    auto evaluated = modules.evaluate("main");
    REQUIRE(!evaluated); REQUIRE(evaluated.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("module dependency cycles link and evaluate once") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"a", "import {b} from \"b\"; export let a=1; export function sum(){return a+b;}"},
        {"b", "import {a} from \"a\"; export let b=2;"},
        {"main", "import {sum} from \"a\"; export let result=sum();"},
    }));
    auto evaluated = modules.evaluate("main"); REQUIRE(evaluated);
    auto result = modules.get_export("main", "result"); REQUIRE(result);
    REQUIRE(result->as_number() == 3.0);
    REQUIRE(modules.module_count() == 3U);
}

TEST_CASE("cyclic module read observes uninitialized live binding") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"a", "import {b} from \"b\"; export let a=b;"},
        {"b", "import {a} from \"a\"; export let b=a;"},
    }));
    auto linked = modules.link("a"); REQUIRE(linked);
    auto evaluated = modules.evaluate("a");
    REQUIRE(!evaluated);
    REQUIRE(evaluated.error().code() == js::ErrorCode::reference_error);
}

TEST_CASE("module environments keep exported heap values alive across GC") {
    js::Runtime runtime; js::Context context(runtime);
    js::module::ModuleSystem modules(context, map_loader({
        {"entry", "export let object={value:9}; export function read(){return object.value;}"},
    }));
    auto evaluated = modules.evaluate("entry"); REQUIRE(evaluated);
    const auto before = runtime.heap_cell_count();
    auto stats = runtime.collect_garbage();
    REQUIRE(stats.after <= before);
    auto object = modules.get_export("entry", "object"); REQUIRE(object); REQUIRE(object->is_object());
    auto value = context.get_property(*object, "value"); REQUIRE(value); REQUIRE(value->as_number() == 9.0);
}

TEST_CASE("ordinary script compilation rejects module declarations") {
    js::Runtime runtime; js::Context context(runtime);
    auto parsed = js::frontend::parse_program("export let x=1;"); REQUIRE(parsed);
    auto compiled = js::compiler::compile_program(context, parsed.program());
    REQUIRE(!compiled); REQUIRE(compiled.error().code() == js::ErrorCode::compile_error);
}

TEST_CASE("module loader receives the referring canonical module name") {
    js::Runtime runtime; js::Context context(runtime);
    std::string seen_referrer;
    js::module::ModuleSystem modules(context,
        [&seen_referrer](std::string_view specifier, std::string_view referrer) -> js::Result<js::module::ModuleSource> {
            if (specifier == "main") return js::module::ModuleSource{"canonical-main", "import {x} from \"dep\"; export let result=x;"};
            if (specifier == "dep") { seen_referrer = std::string(referrer); return js::module::ModuleSource{"canonical-dep", "export let x=5;"}; }
            return js::Error{js::ErrorCode::reference_error, "not found"};
        });
    auto evaluated = modules.evaluate("main"); REQUIRE(evaluated);
    REQUIRE(seen_referrer == "canonical-main");
    auto result = modules.get_export("canonical-main", "result"); REQUIRE(result); REQUIRE(result->as_number() == 5.0);
}
