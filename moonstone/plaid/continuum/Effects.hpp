#pragma once
#include <any>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace moonstone::plaid::continuum {
struct Effect { std::string name; std::any payload; };
using EffectHandler = std::function<std::any(const Effect&)>;
// Synchronous delimited effect dispatch. An effect handler may decide how to
// arrange suspension using a OneShot, but dispatch alone never captures a stack.
class EffectScope {
    std::map<std::string, EffectHandler> handlers_;
    EffectScope* parent_ = nullptr;
public:
    explicit EffectScope(EffectScope* parent = nullptr): parent_(parent) {}
    void bind(std::string name, EffectHandler handler) {
        if (name.empty() || !handler) throw std::invalid_argument("invalid effect handler");
        handlers_.insert_or_assign(std::move(name), std::move(handler));
    }
    std::any perform(Effect effect) const {
        if (auto it = handlers_.find(effect.name); it != handlers_.end()) return it->second(effect);
        if (parent_) return parent_->perform(std::move(effect));
        throw std::runtime_error("unhandled effect: " + effect.name);
    }
};
} // namespace moonstone::plaid::continuum
