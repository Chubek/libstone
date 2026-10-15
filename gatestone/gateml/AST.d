module gateml.AST;

/// All offsets and columns are bytes, not Unicode code points.
struct Span {
    size_t begin, end;
    size_t line = 1, column = 1;
}

Span location(string source, size_t begin, size_t end) {
    import std.algorithm : min;
    Span result = Span(min(begin, source.length), min(end, source.length));
    foreach (c; source[0 .. result.begin]) {
        if (c == '\n') { ++result.line; result.column = 1; }
        else ++result.column;
    }
    return result;
}

class GateError : Exception {
    Span span;
    string source;
    this(string message, Span span = Span.init, string source = null) {
        super(message);
        this.span = span;
        this.source = source;
    }
}

/// Owned tree, retaining punctuation and identity nodes.
class Tree {
    string name, text;
    Span span;
    bool terminal;
    Tree[] children;
    this(string name, Span span = Span.init, Tree[] children = null,
         bool terminal = false, string text = null) {
        this.name = name; this.span = span; this.children = children.dup;
        this.terminal = terminal; this.text = text;
    }
    Tree clone(size_t maxNodes = 1_000_000, size_t maxDepth = 1_024) const {
        validateTree(this, maxNodes, maxDepth);
        Tree copy(const(Tree) node) {
            Tree[] copied;
            foreach (child; node.children) copied ~= copy(child);
            return new Tree(node.name, node.span, copied, node.terminal, node.text);
        }
        return copy(this);
    }
}

/// Validate mutable, caller-supplied trees before recursive processing. Shared
/// subtrees are permitted, but count each occurrence toward the expanded size.
void validateTree(const(Tree) root, size_t maxNodes = 1_000_000, size_t maxDepth = 1_024) {
    struct Frame { const(Tree) node; size_t depth; bool leaving; }
    Frame[] pending = [Frame(root, 1)];
    bool[const(Tree)] active;
    size_t count;
    while (pending.length) {
        auto frame = pending[$ - 1]; pending.length -= 1;
        auto node = frame.node;
        if (node is null) throw new GateError("null tree node");
        if (frame.leaving) { active.remove(node); continue; }
        if (++count > maxNodes) throw new GateError("tree size limit exceeded", node.span);
        if (frame.depth > maxDepth) throw new GateError("tree depth limit exceeded", node.span);
        if (node in active) throw new GateError("cyclic tree", node.span);
        if (node.terminal && node.children.length) throw new GateError("terminal tree nodes cannot have children", node.span);
        if (node.children.length > maxNodes - count) throw new GateError("tree size limit exceeded", node.span);
        active[node] = true;
        pending ~= Frame(node, frame.depth, true);
        foreach_reverse (child; node.children) pending ~= Frame(child, frame.depth + 1);
    }
}
