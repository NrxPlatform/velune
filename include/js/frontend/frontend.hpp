#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <js/frontend/ast.hpp>

namespace js::frontend {

struct SourceLocation {
    std::size_t offset{0};
    std::size_t line{1};
    std::size_t column{0};
};

struct ParseDiagnostic {
    std::string message;
    SourceLocation location;
};

class [[nodiscard]] ParseResult {
public:
    explicit ParseResult(std::unique_ptr<ProgramNode> program)
        : storage_(std::move(program)) {}

    explicit ParseResult(ParseDiagnostic diagnostic)
        : storage_(std::move(diagnostic)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<std::unique_ptr<ProgramNode>>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] ProgramNode& program();
    [[nodiscard]] std::unique_ptr<ProgramNode> take_program();
    [[nodiscard]] const ProgramNode& program() const;
    [[nodiscard]] const ParseDiagnostic& diagnostic() const;

private:
    std::variant<std::unique_ptr<ProgramNode>, ParseDiagnostic> storage_;
};

// Stable frontend entry point. The returned AST contains string_views into
// `source`, so source must outlive the ParseResult/program that is inspected.
[[nodiscard]] ParseResult parse_program(std::string_view source);

} // namespace js::frontend
