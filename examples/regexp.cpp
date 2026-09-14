#include <iostream>
#include <string_view>
#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);
    constexpr std::string_view source = R"(
        let re = /(ab)+/i;
        let matched = re.test("xxABabyy");
        let result = re.exec("xxABabyy");
        let score = 0;
        if (matched) { score = result.index + result[0].length; }
        score
    )";
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) { std::cerr << parsed.diagnostic().message << '\n'; return 1; }
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) { std::cerr << chunk.error() << '\n'; return 1; }
    js::VM vm(context);
    auto value = vm.run(*chunk);
    if (!value) { std::cerr << value.error() << '\n'; return 1; }
    std::cout << "regexp result: " << value->to_debug_string() << '\n';
    std::cout << "runtime heap cells retained: " << runtime.heap_cell_count() << '\n';
}
