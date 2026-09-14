#include <iostream>
#include <string_view>

#include <js/js.hpp>

int main() {
    constexpr std::string_view source = R"JS(
        function fact(n) {
            if (n <= 1) return 1;
            return n * fact(n - 1);
        }
        fact(5)
    )JS";

    js::Runtime runtime;
    js::Context context(runtime);
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) {
        std::cerr << parsed.diagnostic().message << '\n';
        return 1;
    }
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) {
        std::cerr << chunk.error().message() << '\n';
        return 1;
    }

    js::bytecode::Disassembler disassembler;
    auto text = disassembler.disassemble(*chunk);
    if (!text) {
        std::cerr << text.error().message() << '\n';
        return 1;
    }

    std::cout << "source:\n" << source << "\n== program bytecode ==\n" << *text;
    js::VM vm(context);
    const auto result = vm.run(*chunk);
    if (!result) {
        std::cerr << result.error().message() << '\n';
        return 1;
    }
    std::cout << "\nresult: " << result->to_debug_string() << '\n';
    std::cout << "maximum active frames: " << vm.maximum_frame_depth() << '\n';
    return 0;
}
