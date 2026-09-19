#pragma once

#include <string>
#include <string_view>

#include <js/completion.hpp>

namespace js {
class Context;
class GlobalEnvironmentRecord;
namespace detail { struct HeapDynamicEnvironment; struct HeapUpvalue; }

// A selected binding, not a recipe for resolving the name again. The caller
// must keep the active/captured environment rooted while retaining this value.
struct DynamicBindingReference final {
    enum class Target { ObjectEnvironment, StaticBinding, GlobalEnvironment, Unresolvable };
    Target target{Target::Unresolvable};
    detail::HeapDynamicEnvironment* environment{nullptr};
    // A local is captured into a HeapUpvalue before being used as a fallback.
    // This preserves slot identity across frame relocation and closure exit.
    detail::HeapUpvalue* static_binding{nullptr};
    GlobalEnvironmentRecord* global{nullptr};
    std::string name;
    bool strict{false};

    [[nodiscard]] ExecutionResult get(Context& context) const;
    [[nodiscard]] ExecutionResult put(Context& context, Value value) const;
    [[nodiscard]] ExecutionResult delete_binding(Context& context) const;
};

// The compiler supplies an optional already-resolved local/upvalue binding.
// Dynamic objects are checked first; if none binds the name, that exact
// binding is selected before considering the global environment.
[[nodiscard]] ExecutionResult resolve_dynamic_binding(
    Context& context, detail::HeapDynamicEnvironment* active,
    GlobalEnvironmentRecord& global, std::string_view name, bool strict,
    DynamicBindingReference& output,
    detail::HeapUpvalue* static_fallback = nullptr);
} // namespace js
