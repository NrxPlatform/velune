#pragma once

#include <cstdint>
#include <cstddef>

namespace js {

using AtomId = std::uint32_t;
using SymbolId = std::uint32_t;

struct PropertyKey final {
    enum class Kind : std::uint8_t { Atom, Symbol };

    [[nodiscard]] static constexpr PropertyKey atom(AtomId id) noexcept { return PropertyKey{Kind::Atom, id}; }
    [[nodiscard]] static constexpr PropertyKey symbol(SymbolId id) noexcept { return PropertyKey{Kind::Symbol, id}; }

    [[nodiscard]] constexpr bool is_atom() const noexcept { return kind == Kind::Atom; }
    [[nodiscard]] constexpr bool is_symbol() const noexcept { return kind == Kind::Symbol; }
    [[nodiscard]] constexpr AtomId atom_id() const noexcept { return id; }
    [[nodiscard]] constexpr SymbolId symbol_id() const noexcept { return id; }

    friend constexpr bool operator==(PropertyKey, PropertyKey) noexcept = default;

    Kind kind{Kind::Atom};
    std::uint32_t id{0};
};

struct PropertyKeyHash final {
    [[nodiscard]] std::size_t operator()(PropertyKey key) const noexcept {
        return (static_cast<std::size_t>(key.id) << 1U) ^ static_cast<std::size_t>(key.kind == PropertyKey::Kind::Symbol);
    }
};

} // namespace js
