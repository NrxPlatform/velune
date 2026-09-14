#include <iostream>
#include <string>

#include <js/frontend/ast.hpp>
#include <js/frontend/frontend.hpp>

int main() {
    const std::string source =
        "let x = 2; if (x < 4) { x = x * 2 + 1; }";

    auto result = js::frontend::parse_program(source);
    if (!result) {
        const auto& diagnostic = result.diagnostic();
        std::cerr << diagnostic.location.line << ':'
                  << diagnostic.location.column << ": "
                  << diagnostic.message << '\n';
        return 1;
    }

    js::frontend::print_ast(&result.program());
    return 0;
}
