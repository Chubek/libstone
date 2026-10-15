module gateml.Rewrite;
import gateml.AST;

alias RewriteRule = Tree delegate(Tree);
/// Bottom-up, non-mutating rewrite. Rules run once per node in declaration order.
/// A null result means no match; replacement nodes are not recursively revisited.
Tree rewrite(Tree root, RewriteRule[] rules) {
    validateTree(root);
    foreach (rule; rules) if (rule is null) throw new GateError("null rewrite rule");
    Tree transform(Tree node) {
        Tree[] children;
        foreach (child; node.children) children ~= transform(child);
        auto result = new Tree(node.name, node.span, children, node.terminal, node.text);
        foreach (rule; rules) {
            auto replacement = rule(result);
            if (replacement !is null) result = replacement;
        }
        return result;
    }
    auto result = transform(root);
    validateTree(result);
    return result;
}
