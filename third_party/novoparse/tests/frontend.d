module frontend_tests;

import ast : Diagnostic;
import compiler : compileGrammar;
import ir : readIR, writeIR;
import emitter : emit;
import profile : loadProfile, readProfile;
import std.algorithm.searching : canFind;
import std.exception : assertThrown;

unittest
{
    auto grammar = compileGrammar(`
        %grammar Words;
        %start document;
        %skip /[ \t\n]+/;
        %token WORD /[a-z]+/;
        document: WORD ("," WORD)* "."? => Document;
    `);
    assert(grammar.name == "Words");
    assert(writeIR(readIR(writeIR(grammar))) == writeIR(grammar));
    assert(emit(grammar, loadProfile("C")).canFind("Words_create"));
    assert(emit(grammar, loadProfile("Python")).canFind("class WordsParser"));
}

unittest
{
    foreach (source; [
        `S: UNDEFINED;`, `S: "";`, `%start Missing; S: "x";`,
        `S: S;`, `S: "x"; S: "y";`, `%token S "x"; S: "x";`,
        `%unknown x; S: "x";`, `%rewrite nonexistent; S: "x";`,
        `%disambiguate maybe; S: "x";`, `S: ("x"?)*;`,
        `S: "unterminated;`, `S: "x"`, `%left S; S: "x";`,
        `%left "+"; %right "+"; S: "x";`, `__np_reserved: "x";`
    ]) assertThrown!Diagnostic(compileGrammar(source, "bad.ng"));
    try { compileGrammar("S: missing;", "bad.ng"); assert(false); }
    catch (Diagnostic error) { assert(error.msg.canFind("bad.ng:1:4: undefined symbol")); }
}

unittest
{
    auto grammar = compileGrammar(`%token PLUS "+"; %left "+"; S: S PLUS S | "n";`);
    assert(grammar.productions[0].precedence == 1);
    assertThrown!Diagnostic(readIR(writeIR(grammar).replace("(version 1)", "(version 99)")));
    assertThrown!Diagnostic(readProfile(`(profile (version 1) (name "X") (extension ".x") (backend invalid) (template "x"))`));
}

unittest
{
    // Hardening: deterministic Python key order, duplicate rewrites, empty
    // groups, octal-split C quoting, and single-pass template substitution.
    import std.algorithm.searching : canFind;
    import std.string : indexOf;
    auto grammar = compileGrammar(`%grammar Words; S: "x";`);
    auto python = emit(grammar, loadProfile("Python"));
    // pythonTables is S-exp-quoted, so JSON quotes appear escaped.
    assert(python.canFind(`{\"name\":\"Words\",\"start\":`));
    assert(python.indexOf(`\"symbols\"`) < python.indexOf(`\"productions\"`));
    assert(emit(grammar, loadProfile("C")) == emit(grammar, loadProfile("C")));
    assertThrown!Diagnostic(compileGrammar(`%rewrite remove-unit-productions; %rewrite remove-unit-productions; S: "x";`));
    assertThrown!Diagnostic(compileGrammar(`S: ();`));
    assertThrown!Diagnostic(compileGrammar(`S: "x" | "y" => Bad Label;`));
    // Unknown disambiguation must fail loudly instead of coercing to reject.
    import grammar : GrammarIR;
    auto bad = grammar;
    bad.disambiguation = "maybe";
    assertThrown!Diagnostic(emit(bad, loadProfile("C")));
    assertThrown!Diagnostic(emit(bad, loadProfile("Python")));
    assertThrown!Diagnostic(emit(bad, loadProfile("D")));
    // Unknown template placeholders are rejected; literal {{ in templates is
    // only an error when it forms an unknown placeholder.
    auto bogus = readProfile(`(profile (version 1) (name "X") (extension ".x") (backend c) (template "{{bogus}}"))`);
    assertThrown!Diagnostic(emit(grammar, bogus));
    // Future IR versions report unsupported-version even with extra fields.
    auto ir = writeIR(grammar).replace("(version 1)", "(version 2)");
    try { readIR(ir); assert(false); }
    catch (Diagnostic e) { assert(e.msg.canFind("unsupported IR version")); }
}

import std.string : replace;
