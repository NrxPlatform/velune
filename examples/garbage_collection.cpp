#include <iostream>

#include <js/js.hpp>

int main() {
    js::Runtime runtime;
    js::Context context(runtime);

    const auto parent = context.object();
    const auto child = context.object();
    if (!context.set_own_property(parent, "child", child)) return 1;
    if (!context.set_own_property(child, "parent", parent)) return 1;

    auto root_result = runtime.root(parent);
    if (!root_result) return 1;
    auto root = std::move(root_result).value();

    std::cout << "heap before rooted collection: " << runtime.heap_cell_count() << '\n';
    const auto live = runtime.collect_garbage();
    std::cout << "rooted collection: marked=" << live.marked
              << " collected=" << live.collected
              << " remaining=" << live.after << '\n';

    root.reset();
    const auto dead = runtime.collect_garbage();
    std::cout << "after releasing root: marked=" << dead.marked
              << " collected=" << dead.collected
              << " remaining=" << dead.after << '\n';
    return 0;
}
