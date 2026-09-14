#include <iostream>
#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);
    auto parsed = js::frontend::parse_program(R"(
        function* counter() {
            let value = 40;
            yield value;
            value = value + 1;
            yield value;
            return value + 1;
        }
        let it = counter();
        let a = it.next();
        let b = it.next();
        let c = it.next();
        a.value + b.value + c.value
    )");
    if (!parsed) { std::cerr << parsed.diagnostic().message << '\n'; return 1; }
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) { std::cerr << chunk.error().message() << '\n'; return 1; }
    js::VM vm(context);
    auto result = vm.run(*chunk);
    if (!result) { std::cerr << result.error().message() << '\n'; return 1; }
    std::cout << "generator sum: " << result->to_debug_string() << '\n';
    std::cout << "runtime heap cells retained: " << runtime.heap_cell_count() << '\n';
}
