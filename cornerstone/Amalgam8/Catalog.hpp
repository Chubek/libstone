#pragma once

#include "Packaging.hpp"

#include <iterator>

namespace am8 {

namespace catalog {

struct FirestoneHeader {
    std::string_view path, cmake_target;
    bool available;
};

inline constexpr FirestoneHeader firestone_headers[] = {
#include "catalog/firestone.inc"
};

inline const FirestoneHeader* exact_firestone(std::string_view path) {
    const auto begin = std::begin(firestone_headers), end = std::end(firestone_headers);
    const auto it = std::lower_bound(begin, end, path,
        [](const auto& row, std::string_view key) { return row.path < key; });
    return it != end && it->path == path ? it : nullptr;
}

inline const FirestoneHeader& find_firestone(std::string_view name, const Syntax& use) {
    Syntax token = use;
    token.is_atom = true;
    token.text = name;
    include_path(token);
    constexpr std::string_view prefix = "firestone/";
    if (name.starts_with(prefix)) name.remove_prefix(prefix.size());
    if (const auto* found = exact_firestone(name)) return *found;
    // Convenience spelling: libbignum/BigInt, bignum/BigInt, BigInt, BigInt.hpp.
    // Require a unique match, never disambiguate by catalog order.
    std::vector<const FirestoneHeader*> matches;
    for (const auto& row : firestone_headers) {
        const auto slash = row.path.rfind('/');
        const auto leaf = slash == std::string_view::npos ? row.path : row.path.substr(slash + 1);
        const auto stem = [](std::string_view path) { return path.substr(0, path.size() - 4); };
        const auto short_path = row.path.starts_with("lib") ? row.path.substr(3) : row.path;
        if (name == stem(row.path) || name == short_path || name == stem(short_path) ||
            name == leaf || name == stem(leaf))
            matches.push_back(&row);
    }
    if (matches.size() == 1) return *matches.front();
    if (!matches.empty()) {
        std::string message = "ambiguous Firestone header '" + std::string(name) + "'; use a full path:";
        for (const auto* row : matches) message += " " + std::string(row->path);
        fail(use, std::move(message));
    }
    fail(use, "unknown Firestone public header '" + std::string(name) +
              "'; see catalog/firestone.tsv");
}

} // namespace catalog

inline Dependency resolve_firestone(std::string_view name, const Syntax& use,
                                    const Options& options) {
    const auto& row = catalog::find_firestone(name, use);
    Dependency dependency;
    dependency.name = "firestone:" + std::string(row.path);
    dependency.header = "firestone/" + std::string(row.path);
    dependency.context = Context::cpp;
    dependency.span = use.span;
    dependency.conditions = row.available ? "available: installed CMake consumer probe"
                                          : "unverified: public CMake install rules only";
    if (const auto* snapshot = packaging::eligible(row.path)) {
        if (!snapshot->child.empty())
            dependency.requires_names.emplace_back("firestone:" + std::string(snapshot->child));
        if (options.bundle) {
            dependency.bundled_source = packaging::bundle(*snapshot, use, options);
            dependency.kind = DependencyKind::bundled;
            return dependency;
        }
    }
    dependency.include_roots.emplace_back(".");
    dependency.libraries.emplace_back(row.cmake_target);
    return dependency;
}

} // namespace am8
