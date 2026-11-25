#pragma once

#include "WIT-IDL.hpp"

#include <algorithm>

namespace limestone::wasmutils {

class WitVisitor {
public:
    virtual ~WitVisitor() = default;
    virtual void visit(const WitDocument&) {}
    virtual void visit(const WitPackage&) {}
    virtual void visit(const WitVersion&) {}
    virtual void visit(const WitInterface&) {}
    virtual void visit(const WitFunction&) {}
    virtual void visit(const WitParameter&) {}
    virtual void visit(const WitType&) {}
    virtual void visit(const WitName&) {}
    virtual void visit(const WitAnnotations&) {}
    virtual void visit(const WitDocComment&) {}
    virtual void visit(const WitAttribute&) {}
    virtual void visit(const WitAttributeArgument&) {}
    virtual void visit(const WitPath&) {}
    virtual void visit(const WitStandaloneUse&) {}
    virtual void visit(const WitUse&) {}
    virtual void visit(const WitUseItem&) {}
    virtual void visit(const WitTypeDef&) {}
    virtual void visit(const WitCase&) {}
    virtual void visit(const WitWorld&) {}
    virtual void visit(const WitWorldItem&) {}
    virtual void visit(const WitInclude&) {}
};

namespace detail {

class WitWalker {
public:
    explicit WitWalker(WitVisitor& visitor) : visitor_(visitor) {}

    void walk(const WitDocument& node) {
        visitor_.visit(node);
        optional(node.package);
        merged(node.interfaces, node.worlds, node.uses);
    }

private:
    WitVisitor& visitor_;

    struct Entry {
        std::size_t offset;
        const void* node;
        void (*walk)(WitWalker&, const void*);
    };

    // A declaration's name anchors source order between the separate
    // vectors in the AST. Ties (common in hand-built ASTs) are stable.
    template<class T>
    static std::size_t offset(const T& node) { return node.name.location.offset; }

    static std::size_t offset(const WitDocComment& node) { return node.location.offset; }

    static std::size_t offset(const WitPath& node) {
        return node.package ? node.package->name_space.location.offset : offset(node.name);
    }

    static std::size_t offset(const WitName& node) { return node.location.offset; }
    static std::size_t offset(const WitStandaloneUse& node) { return offset(node.path); }
    static std::size_t offset(const WitUse& node) { return offset(node.path); }
    static std::size_t offset(const WitInclude& node) { return offset(node.path); }

    static std::size_t offset(const WitWorldItem& node) {
        if (node.path)
            return offset(*node.path);
        if (node.function)
            return offset(*node.function);
        return node.interface ? offset(*node.interface) : 0;
    }

    template<class T>
    static void add(std::vector<Entry>& entries, const std::vector<T>& nodes) {
        for (const auto& node : nodes) {
            entries.push_back({offset(node), &node, [](WitWalker& walk, const void* ptr) {
                walk.walk(*static_cast<const T*>(ptr));
            }});
        }
    }

    template<class... T>
    void merged(const std::vector<T>&... nodes) {
        std::vector<Entry> entries;
        entries.reserve((nodes.size() + ...));
        (add(entries, nodes), ...);
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
            return left.offset < right.offset;
        });
        for (const auto& entry : entries)
            entry.walk(*this, entry.node);
    }

    template<class T>
    void optional(const std::optional<T>& node) {
        if (node)
            walk(*node);
    }

    template<class T>
    void all(const std::vector<T>& nodes) {
        for (const auto& node : nodes)
            walk(node);
    }

    void walk(const WitName& node) { visitor_.visit(node); }
    void walk(const WitVersion& node) { visitor_.visit(node); }
    void walk(const WitDocComment& node) { visitor_.visit(node); }

    void walk(const WitAnnotations& node) {
        if (!node.docs.empty() || !node.attributes.empty()) {
            visitor_.visit(node);
            merged(node.docs, node.attributes);
        }
    }

    void walk(const WitAttribute& node) {
        visitor_.visit(node);
        walk(node.name);
        all(node.arguments);
    }

    void walk(const WitAttributeArgument& node) {
        visitor_.visit(node);
        walk(node.name);
    }

    void walk(const WitPackage& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.name_space);
        walk(node.name);
        optional(node.version);
    }

    void walk(const WitInterface& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.name);
        merged(node.functions, node.types, node.uses);
    }

    void walk(const WitFunction& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.name);
        all(node.parameters);
        optional(node.result);
    }

    void walk(const WitParameter& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.name);
        walk(node.type);
    }

    void walk(const WitType& node) {
        visitor_.visit(node);
        optional(node.name);
        all(node.arguments);
    }

    void walk(const WitTypeDef& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.name);
        optional(node.alias);
        all(node.fields);
        all(node.cases);
        all(node.functions);
    }

    void walk(const WitCase& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.name);
        optional(node.payload);
    }

    void walk(const WitPath& node) {
        visitor_.visit(node);
        if (const auto& package = node.package) {
            visitor_.visit(*package);
            walk(package->annotations);
            walk(package->name_space);
            walk(package->name);
        }
        walk(node.name);
        if (node.package)
            optional(node.package->version);
    }

    void walk(const WitStandaloneUse& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.path);
        optional(node.alias);
    }

    void walk(const WitUse& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.path);
        all(node.items);
    }

    void walk(const WitUseItem& node) {
        visitor_.visit(node);
        walk(node.name);
        optional(node.alias);
    }

    void walk(const WitWorld& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.name);
        merged(node.items, node.types, node.uses, node.includes);
    }

    void walk(const WitWorldItem& node) {
        visitor_.visit(node);
        walk(node.annotations);
        optional(node.path);
        optional(node.function);
        optional(node.interface);
    }

    void walk(const WitInclude& node) {
        visitor_.visit(node);
        walk(node.annotations);
        walk(node.path);
        all(node.renames);
    }
};

} // namespace detail

// Visits nodes in source order, each before its children. Overriding a
// callback does not affect traversal.
inline void walk_wit(const WitDocument& document, WitVisitor& visitor) {
    detail::WitWalker(visitor).walk(document);
}

} // namespace limestone::wasmutils
