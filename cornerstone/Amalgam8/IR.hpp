#pragma once

#include "Contracts.hpp"

namespace am8::ir {

inline Type named(std::string name, const Span& span = {}) {
    Type type;
    type.name = std::move(name);
    type.span = span;
    return type;
}

inline Type wrapped(TypeKind kind, Type child, const Span& span = {}) {
    Type result;
    result.kind = kind;
    result.children.push_back(std::move(child));
    result.span = span;
    return result;
}

inline bool same_type(const Type& a, const Type& b) {
    if (a.kind != b.kind || a.name != b.name || a.is_const != b.is_const ||
        a.is_volatile != b.is_volatile || a.extent != b.extent ||
        a.children.size() != b.children.size())
        return false;
    for (std::size_t i = 0; i < a.children.size(); ++i)
        if (!same_type(a.children[i], b.children[i])) return false;
    return true;
}

} // namespace am8::ir
