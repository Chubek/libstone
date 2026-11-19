module grammar;

import ast : Associativity, Diagnostic, Location;
import std.ascii : isAlpha, isAlphaNum;
import std.algorithm.searching : canFind;
import std.string : startsWith;

enum SymbolKind { nonterminal, literal, pattern }

struct Symbol
{
    SymbolKind kind;
    string name;
    string value;
    int precedence;
    Associativity associativity;
}

struct Production
{
    int head;
    int[] body;
    string label;
    int precedence;
    Associativity associativity;
}

struct GrammarIR
{
    string name;
    int start;
    string skip;
    bool skipPattern;
    string disambiguation = "reject";
    string[] rewrites;
    Symbol[] symbols;
    Production[] productions;
}

bool isIdentifier(string name)
{
    if (!name.length || (!isAlpha(name[0]) && name[0] != '_')) return false;
    foreach (c; name[1 .. $]) if (!isAlphaNum(c) && c != '_') return false;
    return true;
}

void validateIR(ref GrammarIR ir, Location location)
{
    void fail(string message) { throw new Diagnostic(location, message); }
    if (!isIdentifier(ir.name)) fail("grammar name must be an identifier");
    if (ir.name.startsWith("__np_")) fail("grammar name uses reserved __np_ prefix");
    if (!ir.symbols.length || !ir.productions.length) fail("grammar has no productions");
    // Bound descriptor sizes so a hostile IR cannot force unbounded emitter
    // output or O(n*m) validation blowups downstream.
    enum maxSymbols = 65536, maxProductions = 262144, maxBody = 256;
    if (ir.symbols.length > maxSymbols) fail("grammar has too many symbols");
    if (ir.productions.length > maxProductions) fail("grammar has too many productions");
    if (ir.start < 0 || ir.start >= ir.symbols.length || ir.symbols[ir.start].kind != SymbolKind.nonterminal)
        fail("start symbol must be a defined nonterminal");
    if (ir.disambiguation != "reject" && ir.disambiguation != "first") fail("unknown disambiguation policy: " ~ ir.disambiguation);
    bool[string] names;
    foreach (symbol; ir.symbols)
    {
        if (!isIdentifier(symbol.name)) fail("symbol name must be an identifier: " ~ symbol.name);
        if (symbol.name.startsWith("__np_") && symbol.kind == SymbolKind.nonterminal)
        {
            // Compiler-generated helpers are allowed through the IR path, but
            // user IR must not spoof ambiguous helper namespaces silently; the
            // compiler itself rejects __np_ in user rules. Accept here for
            // round-trips, since writeIR emits helpers with this prefix.
        }
        if (symbol.name.canFind('\0') || symbol.value.canFind('\0')) fail("symbol names and values must not contain NUL bytes");
        if (symbol.name in names) fail("duplicate symbol: " ~ symbol.name);
        names[symbol.name] = true;
        if (symbol.kind != SymbolKind.nonterminal && !symbol.value.length) fail("terminal must not match the empty string: " ~ symbol.name);
        if (symbol.precedence < 0) fail("precedence must not be negative");
        if (symbol.precedence > 1024) fail("precedence level is too large");
        if ((symbol.precedence == 0) != (symbol.associativity == Associativity.none)) fail("precedence and associativity must be specified together");
    }
    if (ir.skip.canFind('\0')) fail("skip expression contains a NUL byte");
    auto productive = new bool[ir.symbols.length];
    foreach (i, symbol; ir.symbols) productive[i] = symbol.kind != SymbolKind.nonterminal;
    foreach (production; ir.productions)
    {
        if (production.head < 0 || production.head >= ir.symbols.length || ir.symbols[production.head].kind != SymbolKind.nonterminal)
            fail("production head must be a defined nonterminal");
        if (production.body.length > maxBody) fail("production body is too long");
        foreach (id; production.body) if (id < 0 || id >= ir.symbols.length) fail("undefined symbol ID in production");
        if (production.precedence < 0 || (production.precedence == 0) != (production.associativity == Associativity.none)) fail("invalid production precedence");
        if (production.label.length && !isIdentifier(production.label)) fail("node label must be an identifier");
    }
    bool changed = true;
    while (changed)
    {
        changed = false;
        foreach (production; ir.productions)
        {
            bool all = true;
            foreach (id; production.body) all = all && productive[id];
            if (all && !productive[production.head]) { productive[production.head] = true; changed = true; }
        }
    }
    if (!productive[ir.start]) fail("start symbol cannot derive a finite input");
}
