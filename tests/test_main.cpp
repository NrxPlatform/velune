#include "test.hpp"

int main() {
    int failures = 0;
    for (const auto& test_case : test::registry()) {
        try {
            test_case.function();
            std::cout << "[pass] " << test_case.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test_case.name << ": " << error.what() << '\n';
        }
    }
    std::cout << test::registry().size() - static_cast<std::size_t>(failures)
              << "/" << test::registry().size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
