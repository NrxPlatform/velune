#include <iostream>
#include <string_view>

#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    constexpr std::string_view source = R"(
        let state = {value: 0};

        function plusOne(x) {
            state.value = state.value + 10;
            return x + 1;
        }

        let result = Promise.resolve(41).then(plusOne);
        state.value = state.value + 1;
        ({state: state, result: result})
    )";

    auto parsed = js::frontend::parse_program(source);
    if (!parsed) { std::cerr << parsed.diagnostic().message << '\n'; return 1; }
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) { std::cerr << chunk.error().message() << '\n'; return 1; }

    js::VM vm(context);
    auto evaluation = vm.run(*chunk);
    if (!evaluation) { std::cerr << evaluation.error().message() << '\n'; return 1; }
    auto root = runtime.root(*evaluation);
    if (!root) { std::cerr << root.error().message() << '\n'; return 1; }

    auto state = context.get_property(*evaluation, "state");
    auto promise = context.get_property(*evaluation, "result");
    if (!state || !promise) return 1;
    auto before = context.get_property(*state, "value");
    if (!before) return 1;

    std::cout << "before jobs: state=" << before->to_debug_string()
              << ", pending_jobs=" << runtime.pending_job_count() << '\n';

    auto ran = runtime.run_jobs(context);
    if (!ran) { std::cerr << ran.error().message() << '\n'; return 1; }
    auto after = context.get_property(*state, "value");
    auto promise_value = context.promise_result(*promise);
    if (!after || !promise_value) return 1;

    std::cout << "after jobs: state=" << after->to_debug_string()
              << ", promise=" << promise_value->to_debug_string()
              << ", jobs_run=" << *ran << '\n';
}
