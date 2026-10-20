#pragma once
#include "ast.hpp"

namespace flintstone::frontend {
template<class Visitor> void visit(Module& module, Visitor&& visitor) {
    for (auto& function : module.functions)
        for (auto& block : function.blocks)
            for (auto& operation : block.operations) visitor(function, block, operation);
}
template<class Visitor> void visit(const Module& module, Visitor&& visitor) {
    for (const auto& function : module.functions)
        for (const auto& block : function.blocks)
            for (const auto& operation : block.operations) visitor(function, block, operation);
}
}
