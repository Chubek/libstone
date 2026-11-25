#include "TypeDAG.hpp"

#include <algorithm>
#include <utility>

namespace wetstone::alias {

TypeId TypeDAG::add_type(std::string name) {
    if (const auto existing = find(name)) return *existing;
    const TypeId id = nodes_.size();
    nodes_.push_back({name, {}});
    try {
        ids_.emplace(std::move(name), id);
    } catch (...) {
        nodes_.pop_back();
        throw;
    }
    return id;
}

std::optional<TypeId> TypeDAG::find(std::string_view name) const {
    if (const auto it = ids_.find(name); it != ids_.end()) return it->second;
    return std::nullopt;
}

std::optional<std::string> TypeDAG::name(TypeId type) const {
    if (!contains(type)) return std::nullopt;
    return nodes_[type].name;
}

bool TypeDAG::add_parent(TypeId child, TypeId parent) {
    if (!contains(child) || !contains(parent)) return false;
    auto& current = nodes_[child].parents;
    if (std::find(current.begin(), current.end(), parent) != current.end()) return true;
    if (is_subtype(parent, child)) return false;
    current.push_back(parent);
    return true;
}

std::vector<TypeId> TypeDAG::parents(TypeId type) const {
    if (!contains(type)) return {};
    return nodes_[type].parents;
}

bool TypeDAG::is_subtype(TypeId child, TypeId parent) const {
    if (!contains(child) || !contains(parent)) return false;
    if (child == parent) return true;

    std::vector<bool> visited(nodes_.size(), false);
    std::vector<TypeId> pending{child};
    visited[child] = true;
    while (!pending.empty()) {
        const auto type = pending.back();
        pending.pop_back();
        for (const auto candidate : nodes_[type].parents) {
            if (candidate == parent) return true;
            if (visited[candidate]) continue;
            visited[candidate] = true;
            pending.push_back(candidate);
        }
    }
    return false;
}

} // namespace wetstone::alias
