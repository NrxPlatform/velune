#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>

#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    const std::unordered_map<std::string, std::string> sources{
        {"counter", "export let count=40; export function inc(){count=count+1; return count;}"},
        {"main", "import {count,inc} from \"counter\"; inc(); inc(); export let answer=count;"},
    };

    js::module::ModuleSystem modules(
        context,
        [&sources](std::string_view specifier, std::string_view referrer) -> js::Result<js::module::ModuleSource> {
            const auto found = sources.find(std::string(specifier));
            if (found == sources.end()) {
                return js::Error{js::ErrorCode::reference_error,
                    "cannot resolve module '" + std::string(specifier) + "' from '" + std::string(referrer) + "'"};
            }
            return js::module::ModuleSource{found->first, found->second};
        });

    const auto evaluated = modules.evaluate("main");
    if (!evaluated) {
        std::cerr << evaluated.error() << '\n';
        return 1;
    }

    const auto answer = modules.get_export("main", "answer");
    const auto live_count = modules.get_export("counter", "count");
    if (!answer || !live_count) return 1;

    std::cout << "main.answer: " << answer->to_debug_string() << '\n';
    std::cout << "counter.count: " << live_count->to_debug_string() << '\n';
    std::cout << "linked modules: " << modules.module_count() << '\n';
    return 0;
}
