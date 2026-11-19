module ast;

import std.format : format;

struct Location
{
    string file;
    size_t line = 1;
    size_t column = 1;

    string toString() const { return format("%s:%s:%s", file, line, column); }
}

class Diagnostic : Exception
{
    Location location;
    this(Location location, string message)
    {
        this.location = location;
        super(location.toString() ~ ": " ~ message);
    }
}

enum ExpressionKind { reference, literal, sequence, choice, optional, zeroOrMore, oneOrMore }

class Expression
{
    ExpressionKind kind;
    string text;
    Expression[] children;
    Location location;

    this(ExpressionKind kind, Location location, string text = "", Expression[] children = [])
    {
        this.kind = kind;
        this.location = location;
        this.text = text;
        this.children = children;
    }
}

struct Alternative
{
    Expression expression;
    string label;
    Expression precedence;
    Location location;
}

struct Rule
{
    string name;
    Alternative[] alternatives;
    Location location;
}

struct TokenDefinition
{
    string name;
    string value;
    bool pattern;
    Location location;
}

enum Associativity { none, left, right, nonassoc }

struct PrecedenceDeclaration
{
    Associativity associativity;
    Expression[] terminals;
    Location location;
}

struct Grammar
{
    string name = "Parser";
    string start;
    string skip;
    bool skipPattern;
    string disambiguation = "reject";
    TokenDefinition[] tokens;
    Rule[] rules;
    PrecedenceDeclaration[] precedences;
    string[] rewrites;
    Location location;
}
