#include "test.hpp"

#include <string_view>

#include <js/js.hpp>

namespace {
js::Result<js::Value> eval_p6(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, "P6 test source failed to parse"};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}

js::Result<js::Value> compile_function(js::Context& context, std::string_view source) {
    return eval_p6(context, source);
}

void expose_global(js::Context& context, std::string_view name, js::Value value) {
    REQUIRE(context.realm().global_environment().create_global_var_binding(std::string(name), value));
    REQUIRE(context.set_own_property(context.global_object(), name, value));
}
}

TEST_CASE("P6 data descriptor enforces writable and configurable invariants") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value object = context.object();
    REQUIRE(context.define_own_property(object, "x", js::PropertyDescriptor::data(context.number(1), false, true, false)));

    js::PropertyDescriptor change_value; change_value.value = context.number(2);
    const auto changed = context.define_own_property(object, "x", change_value);
    REQUIRE(changed); REQUIRE(!*changed);

    js::PropertyDescriptor make_writable; make_writable.writable = true;
    const auto writable = context.define_own_property(object, "x", make_writable);
    REQUIRE(writable); REQUIRE(!*writable);

    js::PropertyDescriptor make_configurable; make_configurable.configurable = true;
    const auto configurable = context.define_own_property(object, "x", make_configurable);
    REQUIRE(configurable); REQUIRE(!*configurable);

    const auto value = context.get_property(object, "x"); REQUIRE(value); REQUIRE(value->as_number() == 1.0);
}

TEST_CASE("P6 configurable property can transition from data to accessor") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value object = context.object();
    REQUIRE(context.set_own_property(object, "marker", context.number(7)));
    REQUIRE(context.set_own_property(object, "x", context.number(1)));
    const auto getter = compile_function(context, "function getter(){ return this.marker; } getter"); REQUIRE(getter);

    js::PropertyDescriptor accessor;
    accessor.get = *getter;
    const auto converted = context.define_own_property(object, "x", accessor);
    REQUIRE(converted); REQUIRE(*converted);
    const auto value = context.get_property(object, "x"); REQUIRE(value); REQUIRE(value->as_number() == 7.0);
}

TEST_CASE("P6 inherited accessor receives original receiver") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value prototype = context.object();
    const auto getter = compile_function(context, "function getter(){ return this.marker; } getter"); REQUIRE(getter);
    REQUIRE(context.define_own_property(prototype, "x", js::PropertyDescriptor::accessor(*getter)));
    const auto child = context.object(prototype); REQUIRE(child);
    REQUIRE(context.set_own_property(*child, "marker", context.number(19)));

    const auto value = context.get_property(*child, "x"); REQUIRE(value); REQUIRE(value->as_number() == 19.0);
}

TEST_CASE("P6 setter executes JavaScript in same VM and uses receiver") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value object = context.object();
    const auto setter = compile_function(context, "function setter(v){ this.seen = v; } setter"); REQUIRE(setter);
    REQUIRE(context.define_own_property(object, "x", js::PropertyDescriptor::accessor(context.undefined(), *setter)));
    expose_global(context, "o", object);

    const auto result = eval_p6(context, "o.x = 42; o.seen");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 42.0);
}

TEST_CASE("P6 getter JavaScript throw remains catchable Completion") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value object = context.object();
    const auto getter = compile_function(context, "function getter(){ throw 91; } getter"); REQUIRE(getter);
    REQUIRE(context.define_own_property(object, "x", js::PropertyDescriptor::accessor(*getter)));
    expose_global(context, "o", object);

    const auto result = eval_p6(context, "let caught = 0; try { o.x; } catch (e) { caught = e; } caught");
    REQUIRE(result); REQUIRE(result->is_number()); REQUIRE(result->as_number() == 91.0);
}

TEST_CASE("P6 preventExtensions blocks creation but preserves existing updates") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value object = context.object();
    REQUIRE(context.set_own_property(object, "x", context.number(1)));
    const auto prevented = context.prevent_extensions(object); REQUIRE(prevented); REQUIRE(*prevented);
    const auto extensible = context.is_extensible(object); REQUIRE(extensible); REQUIRE(!*extensible);

    js::PropertyDescriptor add = js::PropertyDescriptor::data(context.number(2));
    const auto created = context.define_own_property(object, "y", add); REQUIRE(created); REQUIRE(!*created);
    REQUIRE(context.set_own_property(object, "x", context.number(3)));
    const auto x = context.get_property(object, "x"); REQUIRE(x); REQUIRE(x->as_number() == 3.0);
}

TEST_CASE("P6 own property keys preserve insertion order across delete") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value object = context.object();
    REQUIRE(context.set_own_property(object, "a", context.number(1)));
    REQUIRE(context.set_own_property(object, "b", context.number(2)));
    REQUIRE(context.set_own_property(object, "c", context.number(3)));
    const auto deleted = context.delete_property(object, "b"); REQUIRE(deleted); REQUIRE(*deleted);
    REQUIRE(context.set_own_property(object, "d", context.number(4)));
    const auto keys = context.own_property_keys(object); REQUIRE(keys); REQUIRE(keys->size() == 3U);
    REQUIRE((*keys)[0] == context.property_key("a"));
    REQUIRE((*keys)[1] == context.property_key("c"));
    REQUIRE((*keys)[2] == context.property_key("d"));
}

TEST_CASE("P6 GC traces accessor getter and setter values") {
    js::Runtime runtime; js::Context context(runtime);
    const js::Value object = context.object();
    const auto getter = compile_function(context, "function getter(){ return 12; } getter"); REQUIRE(getter);
    const auto setter = compile_function(context, "function setter(v){ return v; } setter"); REQUIRE(setter);
    REQUIRE(context.define_own_property(object, "x", js::PropertyDescriptor::accessor(*getter, *setter)));
    auto root = runtime.root(object); REQUIRE(root);
    (void)runtime.collect_garbage();
    const auto value = context.get_property(root->value(), "x"); REQUIRE(value); REQUIRE(value->as_number() == 12.0);
}
