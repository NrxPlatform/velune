#include <iostream>
#include <string_view>

#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    constexpr std::string_view source = R"(
        let arraySum = 0;
        for (let x of [2, 4, 6]) {
            arraySum = arraySum + x;
        }

        function makeIterator() {
            let i = 0;
            function next() {
                i = i + 1;
                if (i <= 3) return {value: i, done: 0};
                return {value: 0, done: 1};
            }
            return {next: next};
        }

        let custom = {iterator: makeIterator};
        let customSum = 0;
        for (let x of custom) {
            customSum = customSum + x;
        }

        arraySum + customSum
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
    std::cout << "runtime heap cells retained: " << runtime.heap_cell_count() << '\n';
}
