#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <js/property_key.hpp>

namespace js {

class AtomTable final {
public:
    [[nodiscard]] AtomId intern(std::string_view text);
    [[nodiscard]] std::string_view text(AtomId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return atoms_.size(); }

private:
    std::vector<std::string> atoms_;
    std::unordered_map<std::string, AtomId> index_;
};

} // namespace js
