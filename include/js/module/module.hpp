#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <js/result.hpp>
#include <js/value.hpp>

namespace js {
class Context;
namespace module {

struct ModuleSource final {
    std::string name;
    std::string source;
};

using ModuleLoader = std::function<Result<ModuleSource>(std::string_view specifier, std::string_view referrer)>;

class ModuleSystem final {
public:
    ModuleSystem(Context& context, ModuleLoader loader);
    ~ModuleSystem();

    ModuleSystem(const ModuleSystem&) = delete;
    ModuleSystem& operator=(const ModuleSystem&) = delete;
    ModuleSystem(ModuleSystem&&) = delete;
    ModuleSystem& operator=(ModuleSystem&&) = delete;

    [[nodiscard]] Result<void> link(std::string_view entry_specifier);
    [[nodiscard]] Result<Value> evaluate(std::string_view entry_specifier);
    [[nodiscard]] Result<Value> get_export(std::string_view module_name, std::string_view export_name) const;
    [[nodiscard]] std::size_t module_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace module
} // namespace js
