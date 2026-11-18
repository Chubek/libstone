#pragma once

#include "WAT-AST.hpp"

namespace limestone::wasmutils {

class WATVisitor {
public:
    virtual ~WATVisitor() = default;

    // Visit a folded expression's preparatory instructions left-to-right
    // before its enclosing instruction.
    void walk(WATDocument const& document);

protected:
    virtual void visit(WATInstruction const& instruction) = 0;
};

} // namespace limestone::wasmutils
