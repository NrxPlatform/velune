#pragma once

#include <ostream>
#include <string>
#include <string_view>

namespace js {

enum class ErrorCode {
    invalid_argument,
    type_error,
    reference_error,
    runtime_mismatch,
    unsupported,
    compile_error,
    bytecode_error,
    vm_error,
    uncaught_exception,
    internal,
};

class Error {
public:
    Error(ErrorCode code, std::string message);

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] std::string_view code_name() const noexcept;

private:
    ErrorCode code_;
    std::string message_;
};

std::ostream& operator<<(std::ostream& out, const Error& error);

} // namespace js
