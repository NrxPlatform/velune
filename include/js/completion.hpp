#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <variant>

#include <js/error.hpp>
#include <js/result.hpp>
#include <js/value.hpp>

namespace js {

enum class CompletionType : std::uint8_t {
    Normal,
    Throw,
    Return,
    Break,
    Continue,
};

using ControlTarget = std::uint32_t;
inline constexpr ControlTarget no_control_target = std::numeric_limits<ControlTarget>::max();

class Completion final {
public:
    [[nodiscard]] static Completion normal(Value value = Value::undefined()) noexcept {
        return Completion(CompletionType::Normal, value, no_control_target);
    }
    [[nodiscard]] static Completion throw_(Value value) noexcept {
        return Completion(CompletionType::Throw, value, no_control_target);
    }
    [[nodiscard]] static Completion return_(Value value) noexcept {
        return Completion(CompletionType::Return, value, no_control_target);
    }
    [[nodiscard]] static Completion break_(ControlTarget target = no_control_target) noexcept {
        return Completion(CompletionType::Break, Value::undefined(), target);
    }
    [[nodiscard]] static Completion continue_(ControlTarget target = no_control_target) noexcept {
        return Completion(CompletionType::Continue, Value::undefined(), target);
    }

    [[nodiscard]] CompletionType type() const noexcept { return type_; }
    [[nodiscard]] const Value& value() const noexcept { return value_; }
    [[nodiscard]] ControlTarget target() const noexcept { return target_; }
    [[nodiscard]] bool is_normal() const noexcept { return type_ == CompletionType::Normal; }
    [[nodiscard]] bool is_throw() const noexcept { return type_ == CompletionType::Throw; }
    [[nodiscard]] bool is_return() const noexcept { return type_ == CompletionType::Return; }
    [[nodiscard]] bool is_abrupt() const noexcept { return type_ != CompletionType::Normal; }

private:
    Completion(CompletionType type, Value value, ControlTarget target) noexcept
        : type_(type), value_(value), target_(target) {}

    CompletionType type_;
    Value value_;
    ControlTarget target_;
};

enum class EngineFailureCode : std::uint8_t {
    OutOfMemory,
    InvalidBytecode,
    ResourceLimit,
    Interrupted,
    HostContractViolation,
    InternalInvariant,
};

class EngineFailure final {
public:
    EngineFailure(EngineFailureCode code, std::string message)
        : code_(code), legacy_error_(legacy_code_for(code), std::move(message)) {}

    explicit EngineFailure(Error error)
        : code_(classify(error.code())), legacy_error_(std::move(error)) {}

    [[nodiscard]] EngineFailureCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return legacy_error_.message(); }
    [[nodiscard]] const Error& legacy_error() const noexcept { return legacy_error_; }

private:
    [[nodiscard]] static EngineFailureCode classify(ErrorCode code) noexcept {
        switch (code) {
        case ErrorCode::bytecode_error:
            return EngineFailureCode::InvalidBytecode;
        case ErrorCode::invalid_argument:
        case ErrorCode::runtime_mismatch:
        case ErrorCode::unsupported:
        case ErrorCode::compile_error:
        case ErrorCode::type_error:
        case ErrorCode::reference_error:
            return EngineFailureCode::HostContractViolation;
        case ErrorCode::vm_error:
        case ErrorCode::uncaught_exception:
        case ErrorCode::internal:
            return EngineFailureCode::InternalInvariant;
        }
        return EngineFailureCode::InternalInvariant;
    }

    [[nodiscard]] static ErrorCode legacy_code_for(EngineFailureCode code) noexcept {
        switch (code) {
        case EngineFailureCode::InvalidBytecode:
            return ErrorCode::bytecode_error;
        case EngineFailureCode::HostContractViolation:
            return ErrorCode::invalid_argument;
        case EngineFailureCode::OutOfMemory:
        case EngineFailureCode::ResourceLimit:
        case EngineFailureCode::Interrupted:
        case EngineFailureCode::InternalInvariant:
            return ErrorCode::internal;
        }
        return ErrorCode::internal;
    }

    EngineFailureCode code_;
    Error legacy_error_;
};

class [[nodiscard]] ExecutionResult final {
public:
    ExecutionResult(Completion completion) : storage_(completion) {}
    ExecutionResult(EngineFailure failure) : storage_(std::move(failure)) {}
    ExecutionResult(Error error) : storage_(EngineFailure(std::move(error))) {}

    [[nodiscard]] static ExecutionResult normal(Value value = Value::undefined()) noexcept {
        return Completion::normal(value);
    }
    [[nodiscard]] static ExecutionResult throw_(Value value) noexcept {
        return Completion::throw_(value);
    }

    [[nodiscard]] bool has_completion() const noexcept { return std::holds_alternative<Completion>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_completion(); }

    [[nodiscard]] Completion& completion() & {
        assert(has_completion());
        return std::get<Completion>(storage_);
    }
    [[nodiscard]] const Completion& completion() const& {
        assert(has_completion());
        return std::get<Completion>(storage_);
    }
    [[nodiscard]] EngineFailure& error() & {
        assert(!has_completion());
        return std::get<EngineFailure>(storage_);
    }
    [[nodiscard]] const EngineFailure& error() const& {
        assert(!has_completion());
        return std::get<EngineFailure>(storage_);
    }

    [[nodiscard]] Value value() const {
        assert(has_completion());
        return completion().value();
    }

    // Convenience for callers that already checked the completion kind.
    [[nodiscard]] const Value* operator->() const {
        assert(has_completion());
        return &completion().value();
    }
    [[nodiscard]] Value operator*() const {
        assert(has_completion());
        return completion().value();
    }

    // Transitional embedding adapter. Internal VM/semantic code must consume Completion
    // directly; this exists only so pre-P1 embedding helpers can migrate independently.
    [[nodiscard]] operator Result<Value>() const {
        if (!has_completion()) return error().legacy_error();
        if (completion().is_throw()) {
            return Error{ErrorCode::uncaught_exception, "uncaught JavaScript exception: " + completion().value().to_debug_string()};
        }
        if (!completion().is_normal()) return Error{ErrorCode::internal, "non-normal completion escaped execution boundary"};
        return completion().value();
    }

private:
    std::variant<Completion, EngineFailure> storage_;
};

inline std::ostream& operator<<(std::ostream& out, const EngineFailure& failure) {
    return out << failure.legacy_error();
}

[[nodiscard]] inline ExecutionResult execution_from_result(Result<Value> result) {
    if (!result) return result.error();
    return Completion::normal(*result);
}

} // namespace js
