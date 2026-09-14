#include <js/atom_table.hpp>

#include <limits>
#include <stdexcept>

namespace js {

AtomId AtomTable::intern(std::string_view text) {
    const auto found = index_.find(std::string(text));
    if (found != index_.end()) return found->second;
    if (atoms_.size() >= std::numeric_limits<AtomId>::max()) throw std::overflow_error("AtomTable exhausted AtomId space");
    const AtomId id = static_cast<AtomId>(atoms_.size());
    atoms_.emplace_back(text);
    index_.emplace(atoms_.back(), id);
    return id;
}

std::string_view AtomTable::text(AtomId id) const noexcept {
    if (id >= atoms_.size()) return {};
    return atoms_[id];
}

} // namespace js
