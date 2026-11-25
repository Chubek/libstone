#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wetstone::alias {

using TypeId = std::size_t;
inline constexpr TypeId invalid_type = std::numeric_limits<TypeId>::max();

// Explicit subtype metadata (child -> parent, with multiple inheritance).
// This graph is not a language's type-based aliasing policy: unrelated types
// or absent metadata do not establish that accesses cannot alias.
class TypeDAG {
public:
    // Names are unique. Re-adding a name returns its existing ID.
    TypeId add_type(std::string name);
    [[nodiscard]] std::optional<TypeId> find(std::string_view name) const;
    [[nodiscard]] std::optional<std::string> name(TypeId type) const;
    [[nodiscard]] bool contains(TypeId type) const noexcept { return type < nodes_.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    // Returns false for unknown IDs or an edge that would introduce a cycle.
    // An already-present edge succeeds without changing the graph.
    bool add_parent(TypeId child, TypeId parent);
    [[nodiscard]] std::vector<TypeId> parents(TypeId type) const;
    // Reflexive for known types; false for unknown IDs.
    [[nodiscard]] bool is_subtype(TypeId child, TypeId parent) const;

private:
    struct Node {
        std::string name;
        std::vector<TypeId> parents;
    };
    std::vector<Node> nodes_;
    std::map<std::string, TypeId, std::less<>> ids_;
};

} // namespace wetstone::alias
