module app;

import compiler : compileGrammar;
import emitter : emit;
import ir : readIR, writeIR;
import profile : loadProfile;
import std.file : readText, rename, write;
import std.path : extension;
import std.stdio : stderr, stdout, writeln;
import std.string : toLower;
import std.conv : to;
import std.random : uniform;

enum help = `NovoParse 0.1.0 — profile-driven GLR parser generator

Usage:
  novoparse check GRAMMAR.ng
  novoparse compile GRAMMAR.ng [-o GRAMMAR.sexp]
  novoparse generate GRAMMAR.ng|GRAMMAR.sexp --profile NAME [-o FILE]
  novoparse GRAMMAR.ng [--profile NAME] [-o FILE]

Profiles: C, C++, D, Python, or a path to a custom .sexp profile.
Output defaults to stdout; use -o - explicitly for stdout.
`;

int main(string[] args)
{
    try { return run(args[1 .. $]); }
    catch (Exception exception) { stderr.writeln(exception.msg); return 1; }
    catch (Throwable throwable) { stderr.writeln("internal error: ", throwable.msg); return 2; }
}

private int run(string[] args)
{
    if (!args.length) { stdout.write(help); return 0; }
    string command, input, output, target;
    for (size_t i; i < args.length; ++i)
    {
        auto argument = args[i];
        if (argument == "--help" || argument == "-h") { stdout.write(help); return 0; }
        if (argument == "--version") { writeln("NovoParse 0.1.0"); return 0; }
        if (argument == "--list-profiles") { writeln("C\nC++\nD\nPython"); return 0; }
        if (argument == "--profile" || argument == "-p" || argument == "--output" || argument == "-o")
        {
            if (++i == args.length) throw new Exception("missing value for " ~ argument);
            if (argument == "--profile" || argument == "-p")
            {
                if (target.length) throw new Exception("duplicate --profile option");
                target = args[i];
            }
            else
            {
                if (output.length) throw new Exception("duplicate --output option");
                output = args[i];
            }
        }
        else if (argument.length && argument[0] == '-') throw new Exception("unknown option: " ~ argument);
        else if (!command.length && !input.length && (argument == "check" || argument == "compile" || argument == "generate")) command = argument;
        else if (!input.length) input = argument;
        else throw new Exception("unexpected argument: " ~ argument);
    }
    if (!input.length) throw new Exception("a grammar file is required");
    if (!command.length) command = target.length ? "generate" : "compile";
    if (command == "generate" && !target.length) throw new Exception("generate requires --profile");
    if (command != "generate" && target.length) throw new Exception("--profile requires generate");
    string text;
    try { text = readText(input); }
    catch (Exception e) { throw new Exception(input ~ ": cannot read file: " ~ e.msg); }
    if (text.length > 8 * 1024 * 1024) throw new Exception(input ~ ": file is too large");
    auto grammar = input.extension.toLower() == ".sexp" ? readIR(text, input) : compileGrammar(text, input);
    if (command == "check")
    {
        if (output.length) throw new Exception("check does not produce an output file");
        writeln(grammar.name, ": ", grammar.symbols.length, " symbols, ", grammar.productions.length, " productions; valid");
        return 0;
    }
    auto result = command == "generate" ? emit(grammar, loadProfile(target)) : writeIR(grammar);
    if (!output.length || output == "-") stdout.write(result);
    else
    {
        // Atomic write: a failed or interrupted write must not leave a
        // truncated output file behind.
        auto temporary = output ~ ".tmp." ~ uniform!uint.to!string;
        write(temporary, result);
        rename(temporary, output);
    }
    return 0;
}
