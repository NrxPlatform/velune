#include <iostream>
#include <string>

#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    const std::string source =
        "let cleanup = 0; "
        "function fail(n) { if (n <= 0) throw 40; return fail(n - 1); } "
        "function run() { try { return fail(2); } catch (e) { return e + 2; } finally { cleanup = 7; } } "
        "run() + cleanup";

    auto parsed = js::frontend::parse_program(source);
    if (!parsed) {
        std::cerr << parsed.diagnostic().message << '\n';
        return 1;
    }

    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) {
        std::cerr << chunk.error() << '\n';
        return 1;
    }

    js::bytecode::Disassembler disassembler;
    auto listing = disassembler.disassemble(*chunk);
    if (!listing) {
        std::cerr << listing.error() << '\n';
        return 1;
    }

    std::cout << "== program bytecode ==\n" << *listing;

    js::VM vm(context);
    auto result = vm.run(*chunk);
    if (!result) {
        std::cerr << result.error() << '\n';
        return 1;
    }

    std::cout << "result: " << result->to_debug_string() << '\n';
    std::cout << "maximum active frames: " << vm.maximum_frame_depth() << '\n';
    return 0;
}
