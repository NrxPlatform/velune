#include <iostream>
#include <string_view>

#include <js/js.hpp>

namespace {
js::Result<js::Value> eval(js::Context& context, std::string_view source) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) return js::Error{js::ErrorCode::internal, parsed.diagnostic().message};
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) return chunk.error();
    js::VM vm(context);
    return vm.run(*chunk);
}
}

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto result = eval(context,
        "let numeric = '40' - 1;"
        "let text = 'answer=' + (numeric + 3);"
        "let loose = 42 == '42';"
        "let strict = 42 === '42';"
        "({text:text, loose:loose, strict:strict})");
    if (!result) {
        std::cerr << result.error().message() << '\n';
        return 1;
    }

    const auto text = context.get_property(*result, "text");
    const auto loose = context.get_property(*result, "loose");
    const auto strict = context.get_property(*result, "strict");
    if (!text || !loose || !strict) return 1;

    std::cout << "text: " << text->as_string() << '\n';
    std::cout << "42 == '42': " << (loose->as_boolean() ? "true" : "false") << '\n';
    std::cout << "42 === '42': " << (strict->as_boolean() ? "true" : "false") << '\n';
    return 0;
}
