#include <js/error.hpp>

#include <utility>

namespace js {

Error::Error(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

std::string_view Error::code_name() const noexcept {
    switch (code_) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::type_error: return "type_error";
    case ErrorCode::reference_error: return "reference_error";
    case ErrorCode::runtime_mismatch: return "runtime_mismatch";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::compile_error: return "compile_error";
    case ErrorCode::bytecode_error: return "bytecode_error";
    case ErrorCode::vm_error: return "vm_error";
    case ErrorCode::uncaught_exception: return "uncaught_exception";
    case ErrorCode::internal: return "internal";
    }
    return "unknown";
}

std::ostream& operator<<(std::ostream& out, const Error& error) {
    return out << error.code_name() << ": " << error.message();
}

} // namespace js
