#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include <js/js.hpp>

namespace {

enum class Mode {
    execute,
    parse_only,
    compile_only,
};

int evaluate_source(std::string_view source, Mode mode) {
    auto parsed = js::frontend::parse_program(source);
    if (!parsed) {
        const auto& diagnostic = parsed.diagnostic();
        std::cerr << "parse_error at " << diagnostic.location.line << ':' << diagnostic.location.column
                  << ": " << diagnostic.message << '\n';
        return 2;
    }

    if (mode == Mode::parse_only) return 0;

    js::Runtime runtime;
    js::Context context(runtime);
    auto chunk = js::compiler::compile_program(context, parsed.program());
    if (!chunk) {
        std::cerr << chunk.error() << '\n';
        return 3;
    }

    if (mode == Mode::compile_only) return 0;

    js::VM vm(context);
    auto result = vm.run(*chunk);
    if (!result) {
        std::cerr << result.error() << '\n';
        return 4;
    }
    if (result.completion().is_throw()) {
        std::cerr << "uncaught_exception: uncaught JavaScript exception: "
                  << result.completion().value().to_debug_string() << '\n';
        return 4;
    }
    if (!result.completion().is_normal()) {
        std::cerr << "internal: non-normal completion escaped script execution\n";
        return 4;
    }

    std::cout << result.completion().value().to_debug_string() << '\n';
    return 0;
}

int evaluate_file(const char* path, Mode mode) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "host_error: cannot open source file: " << path << '\n';
        return 5;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        std::cerr << "host_error: cannot read source file: " << path << '\n';
        return 5;
    }
    return evaluate_source(buffer.str(), mode);
}

void print_usage(const char* program) {
    std::cerr
        << "usage:\n"
        << "  " << program << " -e <source>\n"
        << "  " << program << " <file.js>\n"
        << "  " << program << " --parse-only <file.js>\n"
        << "  " << program << " --compile-only <file.js>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "-e") {
        return evaluate_source(argv[2], Mode::execute);
    }
    if (argc == 2) {
        return evaluate_file(argv[1], Mode::execute);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--parse-only") {
        return evaluate_file(argv[2], Mode::parse_only);
    }
    if (argc == 3 && std::string_view(argv[1]) == "--compile-only") {
        return evaluate_file(argv[2], Mode::compile_only);
    }

    print_usage(argv[0]);
    return 1;
}
