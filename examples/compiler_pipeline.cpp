#include <iostream>
#include <string_view>

#include <js/js.hpp>

int main() {
    constexpr std::string_view source = "let x = 2; x * 4 + 1";

    js::Runtime runtime;
    js::Context context(runtime);

    auto parsed = js::frontend::parse_program(source);
    if (!parsed) {
        std::cerr << "parse failed: " << parsed.diagnostic().message << '\n';
        return 1;
    }

    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) {
        std::cerr << chunk.error() << '\n';
        return 1;
    }

    js::bytecode::Disassembler disassembler;
    const auto disassembly = disassembler.disassemble(*chunk);
    if (!disassembly) {
        std::cerr << disassembly.error() << '\n';
        return 1;
    }

    std::cout << "source: " << source << "\n\n";
    std::cout << "== compiled bytecode ==\n" << *disassembly;
    std::cout << "locals: " << chunk->local_count() << "\n\n";

    js::VM vm(context);
    const auto result = vm.run(*chunk);
    if (!result) {
        std::cerr << result.error() << '\n';
        return 1;
    }

    std::cout << "result: " << result->to_debug_string() << '\n';
    return 0;
}
