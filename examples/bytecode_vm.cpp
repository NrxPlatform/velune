#include <iostream>

#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    js::bytecode::BytecodeBuilder builder;
    const auto two = builder.add_constant(context.number(2));
    const auto three = builder.add_constant(context.number(3));
    const auto one = builder.add_constant(context.number(1));
    if (!two || !three || !one) {
        std::cerr << "failed to build constant pool\n";
        return 1;
    }

    builder.emit_constant(*two);
    builder.emit_constant(*three);
    builder.emit(js::bytecode::OpCode::multiply);
    builder.emit_constant(*one);
    builder.emit(js::bytecode::OpCode::add);
    builder.emit(js::bytecode::OpCode::return_);

    auto chunk = std::move(builder).finish();

    js::bytecode::Disassembler disassembler;
    const auto listing = disassembler.disassemble(chunk);
    if (!listing) {
        std::cerr << listing.error() << '\n';
        return 1;
    }

    std::cout << "== bytecode ==\n" << *listing;

    js::VM vm(context);
    const auto result = vm.run(chunk);
    if (!result) {
        std::cerr << result.error() << '\n';
        return 1;
    }

    std::cout << "result: " << result->to_debug_string() << '\n';
    return 0;
}
