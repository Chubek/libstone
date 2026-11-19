module disambiguator;

import ast : Associativity, Diagnostic, Location;
import grammar : GrammarIR, SymbolKind;

void inferPrecedence(ref GrammarIR ir)
{
    foreach (ref production; ir.productions)
    {
        if (production.precedence) continue;
        // As in yacc, the rightmost terminal determines a rule's precedence.
        foreach_reverse (id; production.body)
        {
            if (id < 0 || id >= ir.symbols.length)
                throw new Diagnostic(Location("<ir>", 1, 1), "undefined symbol ID in production");
            if (ir.symbols[id].kind == SymbolKind.nonterminal) continue;
            production.precedence = ir.symbols[id].precedence;
            production.associativity = ir.symbols[id].associativity;
            break;
        }
    }
}

Associativity readAssociativity(string text, Location location)
{
    switch (text)
    {
        case "none": return Associativity.none;
        case "left": return Associativity.left;
        case "right": return Associativity.right;
        case "nonassoc": return Associativity.nonassoc;
        default: throw new Diagnostic(location, "unknown associativity: " ~ text);
    }
}
