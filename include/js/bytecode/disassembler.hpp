#pragma once

#include <string>

#include <js/bytecode/chunk.hpp>
#include <js/result.hpp>

namespace js::bytecode {

class Disassembler final {
public:
    [[nodiscard]] Result<std::string> disassemble(const BytecodeChunk& chunk) const;
};

} // namespace js::bytecode
