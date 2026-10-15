module gateml.Sema;
import gateml.Module;
import gateml.Specs : identifier;
import gateml.AbsynSpec;
import gateml.VisitorSpec;
import gateml.Lexer;

int facilityArity(string name) {
    switch (name) {
        case "integer": return 0;
        case "identity", "neg", "not": return 1;
        case "add", "sub", "mul", "sdiv", "srem", "band", "bor", "bxor", "shl", "lshr", "ashr": return 2;
        default: return -1;
    }
}

void validate(const Frontend frontend) {
    import std.string : join;
    bool[string] terminals, skips, heads, nodes, signatures, productive;
    void check(string name, Span span = Span.init) {
        if (!identifier(name)) throw new GateError("invalid or reserved identifier: " ~ name, span);
    }
    check(frontend.name);
    if (!frontend.lexer.length) throw new GateError("frontend requires lexical rules");
    foreach (rule; frontend.lexer) {
        check(rule.name, rule.span);
        if (rule.name in terminals || rule.name in skips) throw new GateError("duplicate lexical rule: " ~ rule.name, rule.span);
        if (rule.skip) skips[rule.name] = true; else terminals[rule.name] = true;
    }
    foreach (rule; frontend.productions) {
        check(rule.head, rule.span);
        if (rule.head in terminals || rule.head in skips) throw new GateError("terminal/nonterminal collision: " ~ rule.head, rule.span);
        heads[rule.head] = true;
    }
    if (!(frontend.start in heads)) throw new GateError("undefined start symbol: " ~ frontend.start);
    foreach (node; frontend.nodes) {
        check(node);
        if (node in nodes) throw new GateError("duplicate AST node: " ~ node);
        nodes[node] = true;
    }
    foreach (rule; frontend.productions) {
        auto signature = rule.head ~ ":" ~ rule.body.join(" ");
        if (signature in signatures) throw new GateError("duplicate production: " ~ rule.head, rule.span);
        signatures[signature] = true;
        if (!(rule.node in nodes)) throw new GateError("undefined AST node: " ~ rule.node, rule.span);
        size_t values;
        foreach (symbol; rule.body) {
            if (!(symbol in terminals) && !(symbol in heads)) throw new GateError("undefined grammar symbol: " ~ symbol, rule.span);
            if (symbol in heads) ++values;
        }
        if (auto hook = rule.node in frontend.lowering) {
            check(*hook, rule.span);
            auto arity = facilityArity(*hook);
            if (arity >= 0 && values != arity) throw new GateError("lowering arity mismatch: " ~ rule.node, rule.span);
            if (*hook == "integer" && rule.body.length != 1) throw new GateError("integer requires one terminal", rule.span);
        }
    }
    foreach (node, hook; frontend.lowering) {
        if (!(node in nodes)) throw new GateError("lowering references undefined AST node: " ~ node);
        check(hook);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        foreach (rule; frontend.productions) {
            bool ready = true;
            foreach (symbol; rule.body) if (!(symbol in terminals) && !(symbol in productive)) ready = false;
            if (ready && !(rule.head in productive)) { productive[rule.head] = true; changed = true; }
        }
    }
    foreach (head, unused; heads) if (!(head in productive)) throw new GateError("nonproductive grammar symbol: " ~ head);
    tokenCodes(frontend);
    new Lexer(frontend.lexer);
    VisitorSpec.fromFrontend(frontend);
}
