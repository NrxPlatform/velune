#include "test.hpp"

#include <js/js.hpp>

TEST_CASE("immediates are portable across runtimes") {
    js::Runtime a;
    js::Runtime b;
    js::Context ca(a);
    js::Context cb(b);

    const auto number = ca.number(3.0);
    REQUIRE(cb.validate(number));
}

TEST_CASE("heap values may cross contexts sharing one runtime") {
    js::Runtime runtime;
    js::Context a(runtime);
    js::Context b(runtime);

    const auto string = a.string("shared runtime");
    REQUIRE(b.validate(string));
}

TEST_CASE("heap values are rejected by another runtime") {
    js::Runtime runtime_a;
    js::Runtime runtime_b;
    js::Context a(runtime_a);
    js::Context b(runtime_b);

    const auto string = a.string("runtime A");
    const auto result = b.validate(string);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::runtime_mismatch);
}

TEST_CASE("native objects preserve identity through copied Values") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto object = context.object();
    const auto alias = object;
    REQUIRE(context.set_own_property(alias, "answer", context.number(42.0)));
    const auto property = context.get_own_property(object, "answer");
    REQUIRE(property);
    REQUIRE(property->is_number());
    REQUIRE(property->as_number() == 42.0);
}

TEST_CASE("missing own property returns undefined") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto object = context.object();
    const auto property = context.get_own_property(object, "missing");
    REQUIRE(property);
    REQUIRE(property->is_undefined());
}

TEST_CASE("object rejects property values from another Runtime") {
    js::Runtime runtime_a;
    js::Runtime runtime_b;
    js::Context a(runtime_a);
    js::Context b(runtime_b);

    const auto object = a.object();
    const auto foreign = b.string("foreign");
    const auto result = a.set_own_property(object, "x", foreign);
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::runtime_mismatch);
}

TEST_CASE("contexts sharing one runtime own distinct realms and globals") {
    js::Runtime runtime;
    js::Context first(runtime);
    js::Context second(runtime);

    REQUIRE(&first.realm() != &second.realm());
    auto same_global = js::abstract_operations::strict_equal(first, first.global_object(), second.global_object()); REQUIRE(same_global); REQUIRE(!*same_global);
    auto same_prototype = js::abstract_operations::strict_equal(first, first.object_prototype(), second.object_prototype()); REQUIRE(same_prototype); REQUIRE(!*same_prototype);

    const auto first_object = first.get_global("Object");
    const auto second_object = second.get_global("Object");
    REQUIRE(first_object && second_object);
    auto same_object_ctor = js::abstract_operations::strict_equal(first, *first_object, *second_object); REQUIRE(same_object_ctor); REQUIRE(!*same_object_ctor);
}

TEST_CASE("cross-realm function execution allocates in the function realm") {
    js::Runtime runtime;
    js::Context first(runtime);
    js::Context second(runtime);

    auto parsed = js::frontend::parse_program("function make() { return {}; } make");
    REQUIRE(parsed);
    auto chunk = js::compiler::compile_program(first, parsed.program());
    REQUIRE(chunk);

    js::VM first_vm(first);
    const auto function_result = first_vm.run(*chunk);
    REQUIRE(function_result);
    REQUIRE(function_result.completion().is_normal());
    const js::Value function = function_result.completion().value();
    REQUIRE(function.is_function());

    js::VM second_vm(second);
    const auto object_result = second_vm.call(function, {});
    REQUIRE(object_result);
    REQUIRE(object_result.completion().is_normal());
    const js::Value object = object_result.completion().value();
    REQUIRE(object.is_object());

    const auto prototype = second.get_prototype(object);
    REQUIRE(prototype);
    auto is_first_prototype = js::abstract_operations::strict_equal(first, *prototype, first.object_prototype()); REQUIRE(is_first_prototype); REQUIRE(*is_first_prototype);
    auto is_second_prototype = js::abstract_operations::strict_equal(second, *prototype, second.object_prototype()); REQUIRE(is_second_prototype); REQUIRE(!*is_second_prototype);
}

TEST_CASE("cross-realm references remain safe across garbage collection") {
    js::Runtime runtime;
    js::Context first(runtime);
    js::Context second(runtime);

    const js::Value first_object = first.object();
    const js::Value second_object = second.object();
    REQUIRE(first.set_own_property(first_object, "foreign", second_object));

    auto root_result = runtime.root(first_object);
    REQUIRE(root_result);
    auto root = std::move(root_result).value();
    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.collected == 0U);

    const auto foreign = first.get_property(root.value(), "foreign");
    REQUIRE(foreign);
    auto same_foreign = js::abstract_operations::strict_equal(second, *foreign, second_object); REQUIRE(same_foreign); REQUIRE(*same_foreign);
    REQUIRE(second.validate(*foreign));
}

TEST_CASE("destroying a context leaves runtime-owned realm values valid") {
    js::Runtime runtime;
    js::PersistentRoot root;
    {
        js::Context first(runtime);
        const js::Value object = first.object();
        REQUIRE(first.set_own_property(object, "answer", first.number(42)));
        auto rooted = runtime.root(object);
        REQUIRE(rooted);
        root = std::move(rooted).value();
    }

    REQUIRE(root.valid());
    const auto stats = runtime.collect_garbage();
    REQUIRE(stats.marked > 0U);

    js::Context second(runtime);
    REQUIRE(second.validate(root.value()));
    const auto answer = second.get_property(root.value(), "answer");
    REQUIRE(answer);
    REQUIRE(answer->is_number());
    REQUIRE(answer->as_number() == 42.0);
    root.reset();
}

TEST_CASE("declarative environment distinguishes uninitialized mutable and immutable bindings") {
    js::DeclarativeEnvironmentRecord environment;

    REQUIRE(environment.create_mutable_binding("x"));
    auto before = environment.get_binding_value("x");
    REQUIRE(!before);
    REQUIRE(before.error().code() == js::ErrorCode::reference_error);

    REQUIRE(environment.initialize_binding("x", js::Value::number(1)));
    REQUIRE(environment.set_mutable_binding("x", js::Value::number(2)));
    auto x = environment.get_binding_value("x");
    REQUIRE(x);
    REQUIRE(x->as_number() == 2.0);

    REQUIRE(environment.create_immutable_binding("y"));
    REQUIRE(environment.initialize_binding("y", js::Value::number(3)));
    auto immutable_write = environment.set_mutable_binding("y", js::Value::number(4));
    REQUIRE(!immutable_write);
    REQUIRE(immutable_write.error().code() == js::ErrorCode::type_error);
}

TEST_CASE("global environment separates lexical and object-style bindings") {
    js::GlobalEnvironmentRecord environment;
    REQUIRE(environment.create_global_var_binding("v", js::Value::number(1)));
    REQUIRE(environment.create_global_lexical_binding("l", false));
    REQUIRE(environment.initialize_lexical_binding("l", js::Value::number(2)));

    auto v = environment.get_binding_value("v");
    auto l = environment.get_binding_value("l");
    REQUIRE(v); REQUIRE(l);
    REQUIRE(v->as_number() == 1.0);
    REQUIRE(l->as_number() == 2.0);

    auto conflict = environment.create_global_var_binding("l", js::Value::number(3));
    REQUIRE(!conflict);
    REQUIRE(conflict.error().code() == js::ErrorCode::compile_error);
}

TEST_CASE("Realm bootstraps Infinity NaN and undefined with immutable non-enumerable non-configurable properties") {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto infinity = context.get_global("Infinity");
    const auto nan = context.get_global("NaN");
    const auto undefined_value = context.get_global("undefined");
    REQUIRE(infinity); REQUIRE(infinity->is_number()); REQUIRE(infinity->as_number() > 0.0);
    REQUIRE(nan); REQUIRE(nan->is_number()); REQUIRE(nan->as_number() != nan->as_number());
    REQUIRE(undefined_value); REQUIRE(undefined_value->is_undefined());

    for (const std::string_view name : {"Infinity", "NaN", "undefined"}) {
        const auto descriptor = context.get_own_property_descriptor(context.global_object(), name);
        REQUIRE(descriptor); REQUIRE(*descriptor);
        REQUIRE(descriptor->value().writable.has_value()); REQUIRE(!*descriptor->value().writable);
        REQUIRE(descriptor->value().enumerable.has_value()); REQUIRE(!*descriptor->value().enumerable);
        REQUIRE(descriptor->value().configurable.has_value()); REQUIRE(!*descriptor->value().configurable);
    }
}
