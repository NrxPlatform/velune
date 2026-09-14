#pragma once

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

#include <js/error.hpp>

namespace js {

template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T> does not support reference types");

public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) : storage_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & {
        assert(has_value());
        return std::get<T>(storage_);
    }
    [[nodiscard]] const T& value() const& {
        assert(has_value());
        return std::get<T>(storage_);
    }
    [[nodiscard]] T&& value() && {
        assert(has_value());
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] Error& error() & {
        assert(!has_value());
        return std::get<Error>(storage_);
    }
    [[nodiscard]] const Error& error() const& {
        assert(!has_value());
        return std::get<Error>(storage_);
    }

    [[nodiscard]] T* operator->() { return &value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }
    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }

private:
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] Error& error() & {
        assert(!ok_);
        return error_;
    }
    [[nodiscard]] const Error& error() const& {
        assert(!ok_);
        return error_;
    }

private:
    Error error_{ErrorCode::internal, "Result<void> has no error"};
    bool ok_{true};
};

} // namespace js

