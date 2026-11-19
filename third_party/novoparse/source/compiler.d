module compiler;

import ast;
import grammar;
import disambiguator : inferPrecedence;
import rewriter : validateRewrites;
import reader : readGrammar;
import std.conv : to;
import std.string : startsWith;

private class Compiler
{
    Grammar source;
    GrammarIR ir;
    int[string] names;
    int[string] literals;
    size_t helperCount;
    int[] repeated;
    enum maxDepth = 256;
    enum maxHelpers = 100000;
    enum maxPrecedence = 1024;

    this(Grammar source) { this.source = source; }

    private void identifier(string name, Location location)
    {
        if (!isIdentifier(name) || name.startsWith("__np_"))
            throw new Diagnostic(location, "invalid or reserved identifier: " ~ name);
    }

    private int add(Symbol symbol)
    {
        if (ir.symbols.length >= 65536) throw new Diagnostic(source.location, "grammar has too many symbols");
        if (helperCount >= maxHelpers) throw new Diagnostic(source.location, "grammar expansion is too large");
        auto id = cast(int) ir.symbols.length;
        ir.symbols ~= symbol;
        return id;
    }

    private int resolve(Expression expression)
    {
        if (expression.kind == ExpressionKind.literal)
        {
            if (!expression.text.length) throw new Diagnostic(expression.location, "empty literal; use %empty instead");
            if (auto found = expression.text in literals) return *found;
            auto id = add(Symbol(SymbolKind.literal, "__np_literal_" ~ literals.length.to!string, expression.text));
            literals[expression.text] = id;
            return id;
        }
        if (auto found = expression.text in names) return *found;
        throw new Diagnostic(expression.location, "undefined symbol: " ~ expression.text);
    }

    private int terminal(Expression expression)
    {
        auto id = resolve(expression);
        if (ir.symbols[id].kind == SymbolKind.nonterminal)
            throw new Diagnostic(expression.location, "precedence requires a terminal");
        return id;
    }

    private int[] lowerSequence(Expression expression, size_t depth = 0)
    {
        if (depth > maxDepth) throw new Diagnostic(expression.location, "expression nesting is too deep");
        if (expression.kind != ExpressionKind.sequence) return [lower(expression, depth + 1)];
        int[] body;
        foreach (child; expression.children) body ~= lower(child, depth + 1);
        if (body.length > 256) throw new Diagnostic(expression.location, "sequence is too long");
        return body;
    }

    private int lower(Expression expression, size_t depth = 0)
    {
        if (depth > maxDepth) throw new Diagnostic(expression.location, "expression nesting is too deep");
        if (expression.kind == ExpressionKind.reference || expression.kind == ExpressionKind.literal) return resolve(expression);
        auto id = add(Symbol(SymbolKind.nonterminal, "__np_ebnf_" ~ (helperCount++).to!string));
        if (expression.kind == ExpressionKind.choice)
        {
            foreach (child; expression.children) ir.productions ~= Production(id, lowerSequence(child, depth + 1));
        }
        else if (expression.kind == ExpressionKind.sequence)
            ir.productions ~= Production(id, lowerSequence(expression, depth + 1));
        else
        {
            if (!expression.children.length)
                throw new Diagnostic(expression.location, "repetition operand is missing");
            auto child = lower(expression.children[0], depth + 1);
            if (expression.kind == ExpressionKind.optional || expression.kind == ExpressionKind.zeroOrMore)
                ir.productions ~= Production(id, []);
            if (expression.kind != ExpressionKind.zeroOrMore) ir.productions ~= Production(id, [child]);
            if (expression.kind != ExpressionKind.optional)
            {
                repeated ~= child;
                ir.productions ~= Production(id, [id, child]);
            }
        }
        return id;
    }

    private void rejectNullableRepetition()
    {
        auto nullable = new bool[ir.symbols.length];
        bool changed = true;
        while (changed)
        {
            changed = false;
            foreach (production; ir.productions)
            {
                bool all = true;
                foreach (id; production.body) all = all && nullable[id];
                if (all && !nullable[production.head]) { nullable[production.head] = true; changed = true; }
            }
        }
        foreach (id; repeated) if (nullable[id])
            throw new Diagnostic(source.location, "repetition operand must not derive the empty string");
    }

    private void compileRules()
    {
        int level;
        foreach (declaration; source.precedences)
        {
            ++level;
            if (level > maxPrecedence) throw new Diagnostic(declaration.location, "too many precedence levels");
            foreach (expression; declaration.terminals)
            {
                auto id = terminal(expression);
                if (ir.symbols[id].precedence) throw new Diagnostic(expression.location, "duplicate precedence declaration");
                ir.symbols[id].precedence = level;
                ir.symbols[id].associativity = declaration.associativity;
            }
        }
        foreach (rule; source.rules)
        {
            foreach (alternative; rule.alternatives)
            {
                auto body = lowerSequence(alternative.expression);
                auto production = Production(names[rule.name], body, alternative.label);
                if (alternative.precedence !is null)
                {
                    auto symbol = ir.symbols[terminal(alternative.precedence)];
                    if (!symbol.precedence) throw new Diagnostic(alternative.location, "%prec terminal has no precedence declaration");
                    production.precedence = symbol.precedence;
                    production.associativity = symbol.associativity;
                }
                ir.productions ~= production;
            }
        }
        inferPrecedence(ir);
        validateIR(ir, source.location);
        rejectNullableRepetition();
    }

    GrammarIR compile()
    {
        identifier(source.name, source.location);
        if (!source.rules.length) throw new Diagnostic(source.location, "grammar has no rules");
        ir.name = source.name;
        ir.skip = source.skip;
        ir.skipPattern = source.skipPattern;
        ir.disambiguation = source.disambiguation;
        ir.rewrites = source.rewrites;
        validateRewrites(ir.rewrites, source.location);
        foreach (rule; source.rules)
        {
            identifier(rule.name, rule.location);
            if (rule.name in names) throw new Diagnostic(rule.location, "duplicate rule: " ~ rule.name);
            names[rule.name] = add(Symbol(SymbolKind.nonterminal, rule.name));
        }
        foreach (token; source.tokens)
        {
            identifier(token.name, token.location);
            if (token.name in names) throw new Diagnostic(token.location, "duplicate symbol: " ~ token.name);
            names[token.name] = add(Symbol(token.pattern ? SymbolKind.pattern : SymbolKind.literal, token.name, token.value));
            if (!token.pattern && !(token.value in literals)) literals[token.value] = names[token.name];
        }
        auto start = source.start.length ? source.start : source.rules[0].name;
        if (auto found = start in names) ir.start = *found;
        else throw new Diagnostic(source.location, "undefined start symbol: " ~ start);
        compileRules();
        return ir;
    }
}

GrammarIR compileGrammar(string source, string file = "<grammar>")
{
    return compileGrammar(readGrammar(source, file));
}

GrammarIR compileGrammar(Grammar source)
{
    return new Compiler(source).compile();
}
