#include <iostream>
#include <string_view>

#include <js/js.hpp>

int main() {
    constexpr std::string_view source = R"JS(
        function inspect(a, b) {
            return arguments.length * 1000 + a * 100 + b * 10 + arguments[2];
        }
        inspect(4, 0, 7)
    )JS";

    js::Runtime runtime;
    js::Context context(runtime);
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) { std::cerr << parsed.diagnostic().message << '\n'; return 1; }
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) { std::cerr << chunk.error().message() << '\n'; return 1; }
    js::VM vm(context);
    auto result = vm.run(*chunk);
    if (!result) { std::cerr << result.error().message() << '\n'; return 1; }
    std::cout << "result: " << result->to_debug_string() << '\n';
    return 0;
}
