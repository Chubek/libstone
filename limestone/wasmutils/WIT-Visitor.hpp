#pragma once

#include "WIT-IDL.hpp"

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
};

// Visits nodes in source order, each before its children. Overriding a
// callback does not affect traversal. No recursive descent is needed.
inline void walk_wit(const WitDocument& document, WitVisitor& visitor) {
    visitor.visit(document);
    if (const auto& package = document.package) {
        visitor.visit(*package);
        visitor.visit(package->name_space);
        visitor.visit(package->name);
        if (package->version)
            visitor.visit(*package->version);
    }
    for (const auto& interface : document.interfaces) {
        visitor.visit(interface);
        visitor.visit(interface.name);
        for (const auto& function : interface.functions) {
            visitor.visit(function);
            visitor.visit(function.name);
            for (const auto& parameter : function.parameters) {
                visitor.visit(parameter);
                visitor.visit(parameter.name);
                visitor.visit(parameter.type);
            }
            if (function.result)
                visitor.visit(*function.result);
        }
    }
}

} // namespace limestone::wasmutils
