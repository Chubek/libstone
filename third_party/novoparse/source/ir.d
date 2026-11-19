module ir;

import ast : Diagnostic, Location;
import grammar;
import sexp;
import disambiguator : readAssociativity;
import rewriter : validateRewrites;
import std.conv : to;

string writeIR(GrammarIR ir)
{
    string result = "(novoparse\n  (version 1)\n  (name " ~ quoteSexp(ir.name) ~ ")\n  (start " ~ ir.start.to!string ~ ")\n";
    auto skipKind = !ir.skip.length ? "none" : ir.skipPattern ? "pattern" : "literal";
    result ~= "  (skip " ~ skipKind ~ " " ~ quoteSexp(ir.skip) ~ ")\n";
    result ~= "  (disambiguation " ~ ir.disambiguation ~ ")\n  (rewrites";
    foreach (name; ir.rewrites) result ~= " " ~ name;
    result ~= ")\n  (symbols\n";
    foreach (symbol; ir.symbols)
        result ~= "    (symbol " ~ symbol.kind.to!string ~ " " ~ quoteSexp(symbol.name) ~ " " ~ quoteSexp(symbol.value)
            ~ " " ~ symbol.precedence.to!string ~ " " ~ symbol.associativity.to!string ~ ")\n";
    result ~= "  )\n  (productions\n";
    foreach (production; ir.productions)
    {
        result ~= "    (production " ~ production.head.to!string ~ " (body";
        foreach (id; production.body) result ~= " " ~ id.to!string;
        result ~= ") " ~ quoteSexp(production.label) ~ " " ~ production.precedence.to!string ~ " " ~ production.associativity.to!string ~ ")\n";
    }
    return result ~ "  )\n)\n";
}

private void readTables(ref GrammarIR result, Sexp symbols, Sexp productions)
{
    foreach (entry; symbols.children[1 .. $])
    {
        entry.form("symbol", 6);
        SymbolKind kind;
        switch (entry.children[1].atom())
        {
            case "nonterminal": kind = SymbolKind.nonterminal; break;
            case "literal": kind = SymbolKind.literal; break;
            case "pattern": kind = SymbolKind.pattern; break;
            default: throw new Diagnostic(entry.location, "unknown symbol kind");
        }
        result.symbols ~= Symbol(kind, entry.children[2].atom(), entry.children[3].atom(), entry.children[4].number(),
            readAssociativity(entry.children[5].atom(), entry.location));
    }
    foreach (entry; productions.children[1 .. $])
    {
        entry.form("production", 6);
        auto body = entry.children[2];
        body.form("body");
        int[] ids;
        foreach (id; body.children[1 .. $]) ids ~= id.number();
        result.productions ~= Production(entry.children[1].number(), ids, entry.children[3].atom(), entry.children[4].number(),
            readAssociativity(entry.children[5].atom(), entry.location));
    }
}

GrammarIR readIR(string source, string file = "<ir>")
{
    auto root = readSexp(source, file);
    if (!root.isList || root.children.length < 2)
        throw new Diagnostic(root.location, "expected (novoparse ...)");
    // Check the version before enforcing the field count so a future v2 with
    // extra fields reports "unsupported IR version" rather than a count error.
    Sexp versionField;
    foreach (field; root.children[1 .. $])
    {
        if (!field.isList || !field.children.length) continue;
        auto head = field.children[0];
        if (!head.isList && !head.quoted && head.text == "version") versionField = field;
    }
    if (versionField is null) throw new Diagnostic(root.location, "missing IR field: version");
    versionField.form("version", 2);
    if (versionField.children[1].number() != 1) throw new Diagnostic(root.location, "unsupported IR version");
    root.form("novoparse", 9);
    Sexp[string] fields;
    foreach (field; root.children[1 .. $])
    {
        if (!field.isList || !field.children.length) throw new Diagnostic(field.location, "expected an IR field");
        auto name = field.children[0].atom();
        if (name in fields) throw new Diagnostic(field.location, "duplicate IR field: " ~ name);
        fields[name] = field;
    }
    Sexp get(string name, size_t count = size_t.max)
    {
        auto found = name in fields;
        if (found is null) throw new Diagnostic(root.location, "missing IR field: " ~ name);
        (*found).form(name, count);
        return *found;
    }
    if (get("version", 2).children[1].number() != 1) throw new Diagnostic(root.location, "unsupported IR version");
    GrammarIR result;
    result.name = get("name", 2).children[1].atom();
    result.start = get("start", 2).children[1].number();
    auto skip = get("skip", 3);
    auto skipKind = skip.children[1].atom();
    if (skipKind != "none" && skipKind != "literal" && skipKind != "pattern") throw new Diagnostic(skip.location, "unknown skip kind");
    result.skip = skip.children[2].atom();
    result.skipPattern = skipKind == "pattern";
    if ((skipKind == "none") != (result.skip.length == 0)) throw new Diagnostic(skip.location, "invalid skip expression");
    result.disambiguation = get("disambiguation", 2).children[1].atom();
    foreach (name; get("rewrites").children[1 .. $]) result.rewrites ~= name.atom();
    readTables(result, get("symbols"), get("productions"));
    validateRewrites(result.rewrites, root.location);
    validateIR(result, root.location);
    return result;
}
