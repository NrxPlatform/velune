#include <iostream>
#include <span>
#include <string_view>

#include <js/js.hpp>

namespace {

js::Result<js::Value> eval(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, parsed.diagnostic().message};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}

js::ExecutionResult native_read(js::Context& context, js::Value this_value, std::span<const js::Value>) {
    return js::execution_from_result(context.get_property(this_value, "inherited"));
}

} // namespace

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto prototype = context.object();
    if (!context.set_own_property(prototype, "inherited", context.number(7))) return 1;
    const auto child = context.object(prototype);
    if (!child) return 1;
    const auto inherited = context.get_property(*child, "inherited");
    if (!inherited) return 1;

    std::cout << "prototype lookup: " << inherited->to_debug_string() << '\n';

    const auto method_result = eval(context,
        "function add(n) { this.x = this.x + n; return this.x; } "
        "let obj = {x: 10, add: add}; obj.add(5)");
    if (!method_result) {
        std::cerr << method_result.error().message() << '\n';
        return 1;
    }
    std::cout << "method this result: " << method_result->to_debug_string() << '\n';

    const auto native = context.native_function("readInherited", 0, &native_read);
    if (!context.set_own_property(prototype, "read", native)) return 1;

    js::bytecode::BytecodeBuilder builder;
    const auto receiver = builder.add_constant(*child);
    const auto key = builder.add_constant(context.string("read"));
    if (!receiver || !key) return 1;
    builder.emit_constant(*receiver);
    builder.emit_method_call(*key, 0);
    builder.emit(js::bytecode::OpCode::return_);
    js::VM vm(context);
    const auto native_result = vm.run(std::move(builder).finish());
    if (!native_result) return 1;
    std::cout << "inherited native method this: " << native_result->to_debug_string() << '\n';
    return 0;
}
