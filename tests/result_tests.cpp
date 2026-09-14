#include "test.hpp"

#include <string>
#include <js/result.hpp>

TEST_CASE("Result carries a successful value") {
    js::Result<int> result{42};
    REQUIRE(result);
    REQUIRE(result.value() == 42);
}

TEST_CASE("Result carries a deterministic error") {
    js::Result<int> result{js::Error{js::ErrorCode::unsupported, "not implemented"}};
    REQUIRE(!result);
    REQUIRE(result.error().code() == js::ErrorCode::unsupported);
    REQUIRE(result.error().message() == "not implemented");
}

TEST_CASE("Result<void> represents success and failure") {
    js::Result<void> ok;
    REQUIRE(ok);

    js::Result<void> failure{js::Error{js::ErrorCode::invalid_argument, "bad input"}};
    REQUIRE(!failure);
    REQUIRE(failure.error().code() == js::ErrorCode::invalid_argument);
}
