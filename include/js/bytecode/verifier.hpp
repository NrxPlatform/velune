#pragma once

#include <cstddef>

#include <js/bytecode/chunk.hpp>
#include <js/result.hpp>

namespace js::bytecode {

struct VerificationInfo {
    std::size_t instruction_count{0};
    std::size_t maximum_stack_depth{0};
};

class BytecodeVerifier final {
public:
    [[nodiscard]] Result<VerificationInfo> verify(const BytecodeChunk& chunk) const;
};

} // namespace js::bytecode
