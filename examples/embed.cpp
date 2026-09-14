#include <iostream>
#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    const js::Value answer = context.number(42.0);
    const js::Value message = context.string("hello from the host");

    std::cout << "answer: " << answer.to_debug_string() << '\n';
    std::cout << "message: " << message.to_debug_string() << '\n';
    return 0;
}
