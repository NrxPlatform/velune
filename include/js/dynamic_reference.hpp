#pragma once

#include <string>
#include <string_view>

#include <js/completion.hpp>

namespace js {
class Context;
class GlobalEnvironmentRecord;
namespace detail { struct HeapDynamicEnvironment; }

// A selected binding, not a recipe for resolving the name again. The caller
// must keep the active/captured environment rooted while retaining this value.
struct DynamicBindingReference final {
    enum class Target { ObjectEnvironment, GlobalEnvironment, Unresolvable };
    Target target{Target::Unresolvable};
    detail::HeapDynamicEnvironment* environment{nullptr};
    GlobalEnvironmentRecord* global{nullptr};
    std::string name;
    bool strict{false};

    [[nodiscard]] ExecutionResult get(Context& context) const;
    [[nodiscard]] ExecutionResult put(Context& context, Value value) const;
    [[nodiscard]] ExecutionResult delete_binding(Context& context) const;
};

// Resolves only the dynamic-object/global segment of a binding chain. Static
// local/upvalue fallback is supplied by the compiler in Patch 2B.
[[nodiscard]] ExecutionResult resolve_dynamic_binding(
    Context& context, detail::HeapDynamicEnvironment* active,
    GlobalEnvironmentRecord& global, std::string_view name, bool strict,
    DynamicBindingReference& output);
} // namespace js
