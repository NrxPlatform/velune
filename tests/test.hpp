#pragma once

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test {

using Function = void (*)();

struct Case {
    const char* name;
    Function function;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, Function function) {
        registry().push_back(Case{name, function});
    }
};

inline void require(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(
            std::string(file) + ":" + std::to_string(line) + ": REQUIRE(" + expression + ") failed");
    }
}

} // namespace test


#define TEST_CASE_IMPL(name, line) \
    static void test_case_##line(); \
    static ::test::Registrar test_registrar_##line{name, &test_case_##line}; \
    static void test_case_##line()
#define TEST_CASE_EXPAND(name, line) TEST_CASE_IMPL(name, line)
#define TEST_CASE(name) TEST_CASE_EXPAND(name, __LINE__)
#define REQUIRE(expr) ::test::require(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
