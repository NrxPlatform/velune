#include <js/frontend/frontend.hpp>

#include <cassert>
#include <exception>
#include <stdexcept>

#include <js/frontend/parser.hpp>
#include <js/frontend/static_semantics.hpp>

namespace js::frontend {

ProgramNode& ParseResult::program() {
    assert(has_value());
    return *std::get<std::unique_ptr<ProgramNode>>(storage_);
}

std::unique_ptr<ProgramNode> ParseResult::take_program() {
    assert(has_value());
    return std::move(std::get<std::unique_ptr<ProgramNode>>(storage_));
}

const ProgramNode& ParseResult::program() const {
    assert(has_value());
    return *std::get<std::unique_ptr<ProgramNode>>(storage_);
}

const ParseDiagnostic& ParseResult::diagnostic() const {
    assert(!has_value());
    return std::get<ParseDiagnostic>(storage_);
}

ParseResult parse_program(std::string_view source) {
    try {
        Parser parser(source);
        auto node = parser.parse_program();
        auto* program = dynamic_cast<ProgramNode*>(node.release());
        if (program == nullptr) {
            return ParseResult(ParseDiagnostic{
                "internal frontend error: parser did not return ProgramNode",
                {0, 1, 0},
            });
        }
        std::unique_ptr<ProgramNode> owned(program);
        if (auto error = validate_static_semantics(*owned)) {
            SourceLocation location{error->offset, 1, error->offset};
            for (std::size_t i = 0, line_start = 0; i < error->offset && i < source.size(); ++i) {
                if (source[i] == '\n') {
                    ++location.line;
                    line_start = i + 1;
                    location.column = error->offset - line_start;
                }
            }
            return ParseResult(ParseDiagnostic{std::move(error->message), location});
        }
        return ParseResult(std::move(owned));
    } catch (const ParseException& error) {
        return ParseResult(ParseDiagnostic{
            error.what(),
            {error.offset, error.line, error.column},
        });
    } catch (const std::exception& error) {
        return ParseResult(ParseDiagnostic{error.what(), {0, 1, 0}});
    }
}

} // namespace js::frontend
