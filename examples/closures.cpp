#include <iostream>
#include <string_view>

#include <js/js.hpp>

int main() {
    constexpr std::string_view source = R"(
        function makeCounter(start) {
            let count = start;
            function next() {
                count = count + 1;
                return count;
            }
            return next;
        }

        let a = makeCounter(10);
        let b = makeCounter(100);
        a() + a() + b()
    )";

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
    const auto disassembly = disassembler.disassemble(*chunk);
    if (!disassembly) {
        std::cerr << disassembly.error().message() << '\n';
        return 1;
    }

    std::cout << "source:\n" << source << "\n== program bytecode ==\n" << *disassembly;
    std::cout << "program local slots: " << chunk->local_count() << '\n';

    js::VM vm(context);
    const auto result = vm.run(*chunk);
    if (!result) {
        std::cerr << result.error().message() << '\n';
        return 1;
    }

    std::cout << "result: " << result->to_debug_string() << '\n';
    std::cout << "maximum active frames: " << vm.maximum_frame_depth() << '\n';
    std::cout << "runtime heap cells retained: " << runtime.heap_cell_count() << '\n';
    return 0;
}
