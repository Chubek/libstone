module gateml.Grammar;
import gateml.AST;
import gateml.Module;
import gateml.Lexer;
import gateml.Sema;
import gateml.TargetSpec;
import std.conv : to;

string grammar(const Frontend frontend) {
    validate(frontend);
    auto codes = tokenCodes(frontend); char[string] terminals;
    foreach (i, rule; frontend.lexer) if (!rule.skip) terminals[rule.name] = codes[i];
    string output = "%grammar " ~ frontend.name ~ ";\n%start " ~ frontend.start ~ ";\n";
    bool[string] seen;
    foreach (first; frontend.productions) {
        if (first.head in seen) continue;
        seen[first.head] = true; output ~= first.head ~ ":"; bool initial = true;
        foreach (rule; frontend.productions) if (rule.head == first.head) {
            if (!initial) output ~= "\n    |";
            initial = false;
            if (!rule.body.length) output ~= " %empty";
            foreach (symbol; rule.body) {
                output ~= " ";
                if (auto code = symbol in terminals) output ~= "\"" ~ *code ~ "\"";
                else output ~= symbol;
            }
            output ~= " => " ~ rule.node;
        }
        output ~= ";\n";
    }
    return output;
}

/// Finite chart fixed point: handles left recursion and epsilon productions.
/// Keep two distinct derivations per symbol/span to detect ambiguity. This is
/// a correctness-first reference parser, not a replacement for native libglr.
Tree parse(const Frontend frontend, string source, TargetSpec target = TargetSpec.init) {
    validate(frontend); target.validate();
    bool[string] reachable;
    reachable[frontend.start] = true;
    bool added = true;
    while (added) {
        added = false;
        foreach (rule; frontend.productions) if (rule.head in reachable)
            foreach (symbol; rule.body) if (!(symbol in reachable)) {
                reachable[symbol] = true; added = true;
            }
    }
    foreach (rule; frontend.productions)
        if (rule.head in reachable && rule.body.length > target.maxProductionSymbols)
            throw new GateError("reference parser production size limit exceeded", rule.span);
    auto lexed = (new Lexer(frontend.lexer)).scan(source, target.maxTokens, target.maxSourceBytes);
    auto count = lexed.tokens.length;
    if (count > target.maxTokens) throw new GateError("reference parser token limit exceeded", lexed.eof);
    struct Candidate { Tree tree; string signature; size_t id, nodes = 1, depth = 1; }
    size_t nextId;
    Candidate[][string] chart;
    string key(string symbol, size_t begin, size_t end) { return symbol ~ ":" ~ begin.to!string ~ ":" ~ end.to!string; }
    Span span(size_t begin, size_t end) {
        auto result = begin < count ? lexed.tokens[begin].span : lexed.eof;
        result.end = end > begin ? lexed.tokens[end - 1].span.end : result.begin;
        return result;
    }
    foreach (i, token; lexed.tokens)
        chart[key(token.name, i, i + 1)] = [Candidate(new Tree(token.name, token.span, null, true, token.text), "t" ~ i.to!string, nextId++)];
    bool changed = true;
    size_t work;
    while (changed) {
        changed = false;
        foreach (p, rule; frontend.productions) foreach (begin; 0 .. count + 1) {
            if (!(rule.head in reachable)) continue;
            void match(size_t index, size_t position, Tree[] children, string signature, size_t nodes, size_t depth) {
                if (++work > target.maxInstructions) throw new GateError("reference parser work limit exceeded", span(position, position));
                if (index == rule.body.length) {
                    auto k = key(rule.head, begin, position);
                    auto existing = k in chart;
                    if (existing !is null) {
                        foreach (candidate; *existing) if (candidate.signature == signature) return;
                        if (existing.length >= 2) return;
                    }
                    chart[k] ~= Candidate(new Tree(rule.node, span(begin, position), children), signature, nextId++, nodes, depth);
                    changed = true; return;
                }
                foreach (end; position .. count + 1) {
                    auto candidates = key(rule.body[index], position, end) in chart;
                    if (candidates is null) continue;
                    // Snapshot because recursive completion can append to this cell.
                    foreach (candidate; (*candidates).dup) {
                        if (candidate.nodes > target.maxTreeNodes - nodes)
                            throw new GateError("reference parser tree size limit exceeded", span(begin, end));
                        if (candidate.depth >= target.maxTreeDepth)
                            throw new GateError("reference parser tree depth limit exceeded", span(begin, end));
                        auto childDepth = candidate.depth + 1;
                        // Flat derivation keys avoid exponentially expanding strings
                        // when nullable productions reuse the same child twice.
                        match(index + 1, end, children ~ candidate.tree, signature ~ "(" ~ candidate.id.to!string ~ ")",
                              nodes + candidate.nodes, depth > childDepth ? depth : childDepth);
                    }
                }
            }
            match(0, begin, null, "p" ~ p.to!string, 1, 1);
        }
    }
    auto roots = key(frontend.start, 0, count) in chart;
    if (roots is null) {
        // Only prefixes reachable from the entry rule determine diagnostics.
        // A nonterminal that happens to parse at offset zero may be legal only
        // after punctuation, and must not shift an initial error to EOF.
        bool[string] terminals, predicted, advanced;
        foreach (rule; frontend.lexer) if (!rule.skip) terminals[rule.name] = true;
        size_t farthest;
        struct Progress { size_t p, index, position; }
        Progress[] pending;
        void advance(size_t p, size_t index, size_t position) {
            auto state = p.to!string ~ ":" ~ index.to!string ~ ":" ~ position.to!string;
            if (state in advanced) return;
            advanced[state] = true;
            pending ~= Progress(p, index, position);
        }
        void predict(string symbol, size_t position) {
            auto state = key(symbol, position, position);
            if (state in predicted) return;
            predicted[state] = true;
            foreach (p, rule; frontend.productions) if (rule.head == symbol) advance(p, 0, position);
        }
        predict(frontend.start, 0);
        while (pending.length) {
            auto item = pending[$ - 1]; pending.length -= 1;
            auto position = item.position;
            if (++work > target.maxInstructions) throw new GateError("reference parser work limit exceeded", span(position, position));
            if (position > farthest) farthest = position;
            auto rule = frontend.productions[item.p];
            if (item.index == rule.body.length) continue;
            auto symbol = rule.body[item.index];
            if (symbol in terminals) {
                if (position < count && lexed.tokens[position].name == symbol) advance(item.p, item.index + 1, position + 1);
            } else {
                predict(symbol, position);
                foreach (end; position .. count + 1)
                    if (key(symbol, position, end) in chart) advance(item.p, item.index + 1, end);
            }
        }
        throw new GateError("syntax error", span(farthest, farthest));
    }
    if (roots.length != 1) throw new GateError("ambiguous input", span(0, count));
    return (*roots)[0].tree.clone(target.maxTreeNodes, target.maxTreeDepth);
}
