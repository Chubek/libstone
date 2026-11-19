module rewriter;

import ast : Diagnostic, Location;
import std.algorithm.searching : canFind;

immutable string[] rewriteNames = [
    "remove-epsilon-productions", "remove-unit-productions", "eliminate-useless-symbols",
    "remove-left-recursion", "left-factor", "make-lr-compatible", "eliminate-ambiguity",
    "remove-duplicate-productions", "remove-self-unit-productions", "eliminate-unreachable-symbols",
    "eliminate-unproductive-symbols", "eliminate-unused-terminals", "augment-start-symbol",
    "isolate-terminals", "left-binarize", "right-binarize", "chomsky-normal-form",
    "right-factor", "remove-right-recursion", "reverse-productions",
    "inline-single-production-nonterminals", "merge-equivalent-nonterminals"
];

void validateRewrites(string[] names, Location location)
{
    foreach (name; names) if (!rewriteNames.canFind(name)) throw new Diagnostic(location, "unknown rewrite: " ~ name);
    bool[string] seen;
    foreach (name; names)
    {
        if (name in seen) throw new Diagnostic(location, "duplicate rewrite: " ~ name);
        seen[name] = true;
    }
}

string rewriteProgram(string[] names)
{
    if (!names.length) return "";
    // Names are allowlisted by validateRewrites; re-check here so hand-built
    // IR passed straight to the emitter cannot inject S-expression syntax.
    foreach (name; names) if (!rewriteNames.canFind(name)) throw new Diagnostic(Location("<ir>", 1, 1), "unknown rewrite: " ~ name);
    string result = "(rewrite (name \"NovoParse\") (rules";
    foreach (name; names) result ~= " (" ~ name ~ ")";
    return result ~ "))";
}
