#include <iostream>
#include <string_view>

#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    constexpr std::string_view source = R"(
        let i = 0;
        let sum = 0;
        while (i < 7) {
            i++;
            if (i == 2) { continue; }
            if (i == 6) { break; }
            sum = sum + i;
        }
        let selected = '' || 'done';
        sum + (selected && 100) + (8 >> 2)
    )";

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
    js::VM vm(context);
    auto result = vm.run(*chunk);
    if (!result) {
        std::cerr << result.error().message() << '\n';
        return 1;
    }
    std::cout << "result: " << result->to_debug_string() << '\n';
    return 0;
}
