#include "Internal.hpp"

namespace gateml {
void optimize(Tree& tree) {
    for (auto& c : tree->children) optimize(c);
    if (tree->kind == Expr::If && tree->children[0]->kind == Expr::Boolean) {
        tree = tree->children[tree->children[0]->number ? 1 : 2]; return;
    }
    auto literal = [](const Tree& e) -> std::optional<Value> {
        if (e->kind == Expr::Integer) return value(Value::Kind::Int, e->number);
        if (e->kind == Expr::Boolean) return value(Value::Kind::Bool, e->number);
        return std::nullopt;
    };
    std::optional<Value> result;
    // Invalid arithmetic is kept as a runtime trap, never a compile-time side
    // effect. In particular, dead short-circuit branches cannot start trapping.
    try {
        if (tree->kind == Expr::Unary) {
            auto a = literal(tree->children[0]); if (a) result = unary(tree->text, *a);
        } else if (tree->kind == Expr::Binary) {
            auto a = literal(tree->children[0]), b = literal(tree->children[1]);
            if (a && b) result = binary(tree->text, *a, *b);
        }
    } catch (const std::runtime_error&) { return; }
    if (!result) return;
    tree->kind = result->kind() == Value::Kind::Bool ? Expr::Boolean : Expr::Integer;
    tree->number = result->data->number; tree->children.clear(); tree->text.clear(); tree->height = 1;
}
} // namespace gateml
