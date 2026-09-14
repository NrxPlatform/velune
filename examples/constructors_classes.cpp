#include <iostream>
#include <string_view>

#include <js/js.hpp>

int main() {
    constexpr std::string_view source = R"(
        class Counter {
            constructor(start) {
                this.value = start;
            }

            add(n) {
                this.value = this.value + n;
                return this.value;
            }
        }

        let counter = new Counter(10);
        counter.add(5) + counter.value
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
    js::VM vm(context);
    auto result = vm.run(*chunk);
    if (!result) {
        std::cerr << result.error().message() << '\n';
        return 1;
    }
    std::cout << "result: " << result->to_debug_string() << '\n';
    std::cout << "maximum active frames: " << vm.maximum_frame_depth() << '\n';
    std::cout << "runtime heap cells retained: " << runtime.heap_cell_count() << '\n';
}
