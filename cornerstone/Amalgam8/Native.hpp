#pragma once

#include "Contracts.hpp"

#include <utility>

namespace am8 {

inline NativeRegistry& NativeRegistry::add(NativeForm form) {
    const auto name = form.name;
    if (name.empty() || !letter(name.front()) ||
        !std::all_of(name.begin(), name.end(), [](char ch) {
            return letter(ch) || digit(ch) || ch == '-';
        }))
        throw std::invalid_argument("invalid native form name '" + name + "'");
    if (form.max_arity && *form.max_arity < form.min_arity)
        throw std::invalid_argument("invalid arity range for native form '" + name + "'");
    if (!form.lower)
        throw std::invalid_argument("native form '" + name + "' requires a lowering callback");
    if (!forms_.try_emplace(name, std::move(form)).second)
        throw std::invalid_argument("duplicate native form '" + name + "'");
    return *this;
}

inline const NativeForm* NativeRegistry::find(std::string_view name) const {
    const auto entry = forms_.find(name);
    return entry == forms_.end() ? nullptr : &entry->second;
}

} // namespace am8
