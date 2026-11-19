module emitter;

import ast : Diagnostic, Location;
import grammar;
import profile : Profile;
import rewriter : rewriteProgram;
import sexp : quoteSexp;
import std.conv : to;
import std.format : format;
import std.json : JSONValue;
import std.string : indexOf;

private string cQuote(string text)
{
    auto bytes = cast(const(ubyte)[]) text;
    string result = "\"";
    foreach (i, c; bytes)
    {
        if (c < 32 || c >= 127)
        {
            result ~= format("\\%03o", c);
            // An octal escape followed by an ASCII octal digit would be
            // re-parsed as a longer escape, so split the C literal there.
            if (i + 1 < bytes.length && bytes[i + 1] >= '0' && bytes[i + 1] <= '7')
                result ~= "\" \"";
        }
        else if (c == '"' || c == '\\') result ~= "\\" ~ cast(char) c;
        else result ~= cast(char) c;
    }
    return result ~ "\"";
}

private string number(T)(T value) { return (cast(long) value).to!string; }

private string cTables(GrammarIR ir)
{
    auto prefix = ir.name;
    string result = "static const np_symbol_spec " ~ prefix ~ "_symbols[] = {\n";
    foreach (symbol; ir.symbols)
        result ~= "    {" ~ cSymbolKind(symbol.kind) ~ ", " ~ cQuote(symbol.name) ~ ", " ~ cQuote(symbol.value)
            ~ ", " ~ number(symbol.precedence) ~ ", " ~ cAssoc(symbol.associativity) ~ "},\n";
    result ~= "};\n";
    foreach (i, production; ir.productions)
    {
        if (!production.body.length) continue;
        result ~= "static const int " ~ prefix ~ "_body_" ~ i.to!string ~ "[] = {";
        foreach (j, id; production.body) result ~= (j ? ", " : "") ~ number(id);
        result ~= "};\n";
    }
    result ~= "static const np_production_spec " ~ prefix ~ "_productions[] = {\n";
    foreach (i, production; ir.productions)
    {
        auto body = production.body.length ? prefix ~ "_body_" ~ i.to!string : "NULL";
        result ~= "    {" ~ number(production.head) ~ ", " ~ body ~ ", " ~ production.body.length.to!string ~ ", " ~ cQuote(production.label)
            ~ ", " ~ number(production.precedence) ~ ", " ~ cAssoc(production.associativity) ~ "},\n";
    }
    result ~= "};\nstatic const np_grammar_spec " ~ prefix ~ "_grammar = {\n    " ~ cQuote(ir.name) ~ ", " ~ prefix ~ "_symbols, " ~ ir.symbols.length.to!string
        ~ ", " ~ prefix ~ "_productions, " ~ ir.productions.length.to!string ~ ", " ~ number(ir.start) ~ ", "
        ~ (!ir.skip.length ? "NP_SKIP_NONE" : ir.skipPattern ? "NP_SKIP_PATTERN" : "NP_SKIP_LITERAL") ~ ", " ~ cQuote(ir.skip) ~ ", " ~ disambiguationEnum(ir)
        ~ ", " ~ cQuote(rewriteProgram(ir.rewrites)) ~ "\n};\n";
    return result;
}

private string cAssoc(T)(T value)
{
    switch (cast(int) value)
    {
        case 0: return "NP_ASSOC_NONE";
        case 1: return "NP_ASSOC_LEFT";
        case 2: return "NP_ASSOC_RIGHT";
        case 3: return "NP_ASSOC_NONASSOC";
        default: throw new Diagnostic(Location("<ir>", 1, 1), "invalid associativity in grammar");
    }
}

private string cSymbolKind(T)(T value)
{
    switch (cast(int) value)
    {
        case 0: return "NP_NONTERMINAL";
        case 1: return "NP_LITERAL";
        case 2: return "NP_PATTERN";
        default: throw new Diagnostic(Location("<ir>", 1, 1), "invalid symbol kind in grammar");
    }
}

private string disambiguationFlag(GrammarIR ir)
{
    if (ir.disambiguation == "first") return "1";
    if (ir.disambiguation == "reject") return "0";
    throw new Diagnostic(Location("<ir>", 1, 1), "unknown disambiguation policy: " ~ ir.disambiguation);
}

private string disambiguationEnum(GrammarIR ir)
{
    if (ir.disambiguation == "first") return "NP_FIRST";
    if (ir.disambiguation == "reject") return "NP_REJECT";
    throw new Diagnostic(Location("<ir>", 1, 1), "unknown disambiguation policy: " ~ ir.disambiguation);
}

private string pythonTables(GrammarIR ir)
{
    // Build the descriptor JSON with a fixed key order. std.json objects are
    // associative arrays, so data.toString() would iterate in an unspecified
    // order and break reproducible output; serialize each value separately.
    string symbols = "[";
    foreach (i, symbol; ir.symbols)
    {
        if (i) symbols ~= ",";
        JSONValue[] row = [JSONValue(cast(int) symbol.kind), JSONValue(symbol.name), JSONValue(symbol.value),
            JSONValue(symbol.precedence), JSONValue(cast(int) symbol.associativity)];
        symbols ~= JSONValue(row).toString();
    }
    symbols ~= "]";
    string productions = "[";
    foreach (i, production; ir.productions)
    {
        if (i) productions ~= ",";
        JSONValue[] body;
        foreach (id; production.body) body ~= JSONValue(id);
        JSONValue[] row = [JSONValue(production.head), JSONValue(body), JSONValue(production.label),
            JSONValue(production.precedence), JSONValue(cast(int) production.associativity)];
        productions ~= JSONValue(row).toString();
    }
    productions ~= "]";
    string json = `{"name":` ~ JSONValue(ir.name).toString()
        ~ `,"start":` ~ JSONValue(ir.start).toString()
        ~ `,"skip_kind":` ~ JSONValue(!ir.skip.length ? 0 : ir.skipPattern ? 2 : 1).toString()
        ~ `,"skip":` ~ JSONValue(ir.skip).toString()
        ~ `,"disambiguation":` ~ disambiguationFlag(ir)
        ~ `,"rewrites":` ~ JSONValue(rewriteProgram(ir.rewrites)).toString()
        ~ `,"symbols":` ~ symbols
        ~ `,"productions":` ~ productions ~ `}`;
    return quoteSexp(json);
}

string emit(GrammarIR ir, Profile target)
{
    string tables;
    switch (target.backend)
    {
        case "c": tables = cTables(ir); break;
        case "d": tables = dTables(ir); break;
        case "python": tables = pythonTables(ir); break;
        default: throw new Diagnostic(Location("<profile>", 1, 1), "unknown backend");
    }
    // Single-pass substitution: only {{name}} and {{tables}} expand. A prior
    // two-pass replace() re-expanded injected text when ir.name contained
    // "{{tables}}", and a canFind("{{") check false-positived on legitimate
    // "{{" in C++/Python templates.
    string result;
    for (size_t i = 0; i < target.templateText.length;)
    {
        auto open = target.templateText[i .. $].indexOf("{{");
        if (open < 0) { result ~= target.templateText[i .. $]; break; }
        result ~= target.templateText[i .. i + cast(size_t) open];
        auto rest = target.templateText[i + cast(size_t) open + 2 .. $];
        auto close = rest.indexOf("}}");
        if (close < 0) throw new Diagnostic(Location("<profile>", 1, 1), "unterminated template placeholder");
        auto placeholder = rest[0 .. cast(size_t) close];
        if (placeholder == "name") result ~= ir.name;
        else if (placeholder == "tables") result ~= tables;
        else throw new Diagnostic(Location("<profile>", 1, 1), "unknown template placeholder: {{" ~ placeholder ~ "}}");
        i += cast(size_t) open + 2 + cast(size_t) close + 2;
    }
    return result;
}

private string dTables(GrammarIR ir)
{
    string result = "    auto symbols = [\n";
    foreach (symbol; ir.symbols)
        result ~= "        SymbolSpec(" ~ number(symbol.kind) ~ ", " ~ quoteSexp(symbol.name) ~ ".ptr, " ~ quoteSexp(symbol.value)
            ~ ".ptr, " ~ number(symbol.precedence) ~ ", " ~ number(symbol.associativity) ~ "),\n";
    result ~= "    ];\n";
    foreach (i, production; ir.productions)
    {
        if (!production.body.length) continue;
        result ~= "    int[] body_" ~ i.to!string ~ " = [";
        foreach (j, id; production.body) result ~= (j ? ", " : "") ~ number(id);
        result ~= "];\n";
    }
    result ~= "    auto productions = [\n";
    foreach (i, production; ir.productions)
    {
        auto body = production.body.length ? "body_" ~ i.to!string ~ ".ptr" : "null";
        result ~= "        ProductionSpec(" ~ number(production.head) ~ ", " ~ body ~ ", " ~ production.body.length.to!string ~ ", " ~ quoteSexp(production.label)
            ~ ".ptr, " ~ number(production.precedence) ~ ", " ~ number(production.associativity) ~ "),\n";
    }
    result ~= "    ];\n    auto spec = GrammarSpec(" ~ quoteSexp(ir.name) ~ ".ptr, symbols.ptr, symbols.length, productions.ptr, productions.length, "
        ~ number(ir.start) ~ ", " ~ number(!ir.skip.length ? 0 : ir.skipPattern ? 2 : 1) ~ ", " ~ quoteSexp(ir.skip) ~ ".ptr, "
        ~ disambiguationFlag(ir) ~ ", " ~ quoteSexp(rewriteProgram(ir.rewrites)) ~ ".ptr);\n";
    return result;
}
