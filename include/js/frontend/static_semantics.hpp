#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <js/frontend/ast.hpp>

namespace js::frontend {

struct StaticSemanticError {
    std::string message;
    std::size_t offset{0};
};

[[nodiscard]] std::optional<StaticSemanticError> validate_static_semantics(const ProgramNode& program);

} // namespace js::frontend
