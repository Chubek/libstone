module sexp;

import ast : Diagnostic, Location;
import reader : Lexer, TokenKind;
import std.conv : to;
import std.format : format;

class Sexp
{
    bool isList;
    bool quoted;
    string text;
    Sexp[] children;
    Location location;

    string atom() const
    {
        if (isList) throw new Diagnostic(location, "expected an atom");
        return text;
    }

    int number() const
    {
        if (isList || quoted) throw new Diagnostic(location, "expected an integer");
        try { return text.to!int; }
        catch (Exception) { throw new Diagnostic(location, "invalid integer: " ~ text); }
    }

    void form(string name, size_t count = size_t.max) const
    {
        if (!isList || !children.length || children[0].isList || children[0].quoted || children[0].text != name)
            throw new Diagnostic(location, "expected (" ~ name ~ " ...)");
        if (count != size_t.max && children.length != count)
            throw new Diagnostic(location, "incorrect field count in " ~ name);
    }
}

Sexp readSexp(string source, string file = "<sexp>")
{
    if (source.length > 8 * 1024 * 1024)
        throw new Diagnostic(Location(file, 1, 1), "S-expression input is too large");
    auto lexer = new Lexer(source, file, true);
    size_t nodes;
    Sexp read(size_t depth)
    {
        if (depth > 256) throw new Diagnostic(lexer.current.location, "S-expression nesting is too deep");
        if (++nodes > 1000000) throw new Diagnostic(lexer.current.location, "S-expression is too large");
        auto node = new Sexp;
        node.location = lexer.current.location;
        if (lexer.accept("("))
        {
            node.isList = true;
            while (!lexer.accept(")"))
            {
                if (lexer.current.kind == TokenKind.end) throw new Diagnostic(node.location, "unterminated S-expression");
                node.children ~= read(depth + 1);
            }
        }
        else
        {
            if (lexer.current.kind == TokenKind.end || lexer.current.text == ")" && lexer.current.kind != TokenKind.quoted)
                throw new Diagnostic(node.location, "expected an S-expression");
            node.quoted = lexer.current.kind == TokenKind.quoted;
            node.text = lexer.current.text;
            lexer.advance();
        }
        return node;
    }
    auto root = read(0);
    if (lexer.current.kind != TokenKind.end) throw new Diagnostic(lexer.current.location, "unexpected text after S-expression");
    return root;
}

/// Canonical quoting shared by the IR and profiles.
/// Emits only \\, \", \n, \r, \t symbolically; all other C0/C1 controls,
/// DEL, and bytes >= 127 are octal-escaped so output round-trips byte-wise
/// and never embeds raw newlines, NUL, or DEL. Iterates raw bytes so UTF-8
/// sequences survive unchanged (each byte >= 128 becomes \ooo).
string quoteSexp(string text)
{
    string result = "\"";
    foreach (ubyte c; cast(const(ubyte)[]) text)
    {
        switch (c)
        {
            case '\\': result ~= "\\\\"; break;
            case '"': result ~= "\\\""; break;
            case '\n': result ~= "\\n"; break;
            case '\r': result ~= "\\r"; break;
            case '\t': result ~= "\\t"; break;
            default:
                if (c < 32 || c >= 127) result ~= format("\\%03o", c);
                else result ~= cast(char) c;
        }
    }
    return result ~ "\"";
}
