module gateml.Visitor;
import gateml.AST;
import gateml.VisitorSpec;

alias VisitHandler = void delegate(Tree);

/// Return false from enter to prune a subtree. leave runs only for entered nodes.
void walk(Tree root, bool delegate(Tree) enter, VisitHandler leave = null) {
    validateTree(root);
    void traverse(Tree node) {
        if (enter !is null && !enter(node)) return;
        foreach (child; node.children) traverse(child);
        if (leave !is null) leave(node);
    }
    traverse(root);
}

void visit(Tree root, const VisitorSpec spec, VisitHandler[string] handlers,
           VisitHandler fallback = null) {
    if (spec.order != "preorder" && spec.order != "postorder") throw new GateError("invalid visitor order");
    void dispatch(Tree node) {
        if (node.terminal && !spec.terminals) return;
        auto name = node.name;
        if (auto mapped = name in spec.handlers) name = *mapped;
        if (auto handler = name in handlers) {
            if (*handler is null) throw new GateError("null visitor handler: " ~ name, node.span);
            (*handler)(node);
        }
        else if (fallback !is null) fallback(node);
    }
    walk(root, (Tree node) { if (spec.order == "preorder") dispatch(node); return true; },
         (Tree node) { if (spec.order == "postorder") dispatch(node); });
}
