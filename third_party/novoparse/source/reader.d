module reader;

import ast;
import std.ascii : isAlpha, isAlphaNum, isDigit, isWhite;

enum TokenKind { identifier, quoted, pattern, integer, punctuation, end }

struct Token
{
    TokenKind kind;
    string text;
    Location location;
}

/// Shared, location-aware reader for grammar files and S-expressions.
class Lexer
{
    private string input;
    private string file;
    private size_t position;
    private size_t line = 1;
    private size_t column = 1;
    private bool sexp;
    Token current;

    this(string input, string file, bool sexp = false)
    {
        this.input = input;
        this.file = file;
        this.sexp = sexp;
        advance();
    }

    private char take()
    {
        auto c = input[position++];
        if (c == '\n') { ++line; column = 1; }
        else ++column;
        return c;
    }

    private bool starts(string text) const
    {
        return position + text.length <= input.length && input[position .. position + text.length] == text;
    }

    void advance()
    {
        while (position < input.length)
        {
            if (isWhite(input[position])) { take(); continue; }
            if ((!sexp && (starts("//") || starts("#"))) || (sexp && starts(";")))
            {
                while (position < input.length && take() != '\n') {}
                continue;
            }
            if (!sexp && starts("/*"))
            {
                auto location = Location(file, line, column);
                take(); take();
                while (position < input.length && !starts("*/")) take();
                if (position == input.length) throw new Diagnostic(location, "unterminated comment");
                take(); take();
                continue;
            }
            break;
        }
        auto location = Location(file, line, column);
        if (position == input.length)
        {
            current = Token(TokenKind.end, "", location);
            return;
        }
        auto c = take();
        if (c == '"' || (!sexp && c == '\''))
        {
            auto quote = c;
            string value;
            while (position < input.length)
            {
                c = take();
                if (c == quote)
                {
                    current = Token(TokenKind.quoted, value, location);
                    return;
                }
                if (c == '\\')
                {
                    if (position == input.length) break;
                    c = take();
                    switch (c)
                    {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case '\\': case '"': case '\'': break;
                        default: throw new Diagnostic(location, "unsupported string escape \\" ~ c);
                    }
                }
                value ~= c;
            }
            throw new Diagnostic(location, "unterminated string");
        }
        if (!sexp && c == '/')
        {
            string value;
            bool inClass;
            while (position < input.length)
            {
                c = take();
                if (c == '/' && !inClass)
                {
                    current = Token(TokenKind.pattern, value, location);
                    return;
                }
                if (c == '\\')
                {
                    if (position == input.length) break;
                    auto next = take();
                    if (next == '/') value ~= '/';
                    else if (next == 'n') value ~= '\n';
                    else if (next == 'r') value ~= '\r';
                    else if (next == 't') value ~= '\t';
                    else value ~= "\\" ~ next;
                    continue;
                }
                if (c == '[') inClass = true;
                if (c == ']') inClass = false;
                value ~= c;
            }
            throw new Diagnostic(location, "unterminated regular expression");
        }
        if (sexp && c != '(' && c != ')')
        {
            auto start = position - 1;
            while (position < input.length && !isWhite(input[position])
                   && input[position] != '(' && input[position] != ')' && input[position] != ';') take();
            current = Token(TokenKind.identifier, input[start .. position], location);
            return;
        }
        if (isAlpha(c) || c == '_')
        {
            auto start = position - 1;
            while (position < input.length && (isAlphaNum(input[position]) || input[position] == '_' || input[position] == '-')) take();
            current = Token(TokenKind.identifier, input[start .. position], location);
            return;
        }
        if (isDigit(c))
        {
            auto start = position - 1;
            while (position < input.length && isDigit(input[position])) take();
            if (position - start > 10)
                throw new Diagnostic(location, "integer literal is too long");
            current = Token(TokenKind.integer, input[start .. position], location);
            return;
        }
        if (c == '=' && starts(">"))
        {
            take(); current = Token(TokenKind.punctuation, "=>", location); return;
        }
        current = Token(TokenKind.punctuation, [c].idup, location);
    }

    bool accept(string text)
    {
        if (current.text != text || current.kind == TokenKind.quoted || current.kind == TokenKind.pattern) return false;
        advance();
        return true;
    }

    Token expect(string text)
    {
        auto token = current;
        if (!accept(text)) throw new Diagnostic(token.location, "expected '" ~ text ~ "'");
        return token;
    }

    Token expectKind(TokenKind kind, string description)
    {
        auto token = current;
        if (token.kind != kind) throw new Diagnostic(token.location, "expected " ~ description);
        advance();
        return token;
    }
}

private class GrammarReader
{
    Lexer lexer;
    Grammar grammar;
    bool[string] directives;

    this(string source, string file)
    {
        lexer = new Lexer(source, file);
        grammar.location = Location(file, 1, 1);
    }

    private void unique(Token directive)
    {
        if (directive.text in directives) throw new Diagnostic(directive.location, "duplicate %" ~ directive.text ~ " directive");
        directives[directive.text] = true;
    }

    private Expression terminal()
    {
        auto token = lexer.current;
        if (token.kind != TokenKind.identifier && token.kind != TokenKind.quoted)
            throw new Diagnostic(token.location, "expected a token name or literal");
        lexer.advance();
        return new Expression(token.kind == TokenKind.quoted ? ExpressionKind.literal : ExpressionKind.reference, token.location, token.text);
    }

    private Expression sequence(size_t depth = 0)
    {
        if (depth > 256) throw new Diagnostic(lexer.current.location, "expression nesting is too deep");
        auto location = lexer.current.location;
        Expression[] children;
        while (lexer.current.kind == TokenKind.identifier || lexer.current.kind == TokenKind.quoted || lexer.current.text == "(")
        {
            Expression child;
            if (lexer.accept("("))
            {
                if (lexer.current.text == ")")
                    throw new Diagnostic(location, "empty group is not allowed; use %empty instead");
                Expression[] choices = [sequence(depth + 1)];
                while (lexer.accept("|")) choices ~= sequence(depth + 1);
                lexer.expect(")");
                child = new Expression(ExpressionKind.choice, location, "", choices);
            }
            else child = terminal();
            if (lexer.accept("?")) child = new Expression(ExpressionKind.optional, child.location, "", [child]);
            else if (lexer.accept("*")) child = new Expression(ExpressionKind.zeroOrMore, child.location, "", [child]);
            else if (lexer.accept("+")) child = new Expression(ExpressionKind.oneOrMore, child.location, "", [child]);
            children ~= child;
        }
        return new Expression(ExpressionKind.sequence, location, "", children);
    }

    private void directive()
    {
        auto token = lexer.expectKind(TokenKind.identifier, "directive name");
        switch (token.text)
        {
            case "grammar": unique(token); grammar.name = lexer.expectKind(TokenKind.identifier, "grammar name").text; break;
            case "start": unique(token); grammar.start = lexer.expectKind(TokenKind.identifier, "start rule").text; break;
            case "disambiguate": unique(token); grammar.disambiguation = lexer.expectKind(TokenKind.identifier, "disambiguation policy").text; break;
            case "skip":
                unique(token);
                auto value = lexer.current;
                if (value.kind != TokenKind.pattern && value.kind != TokenKind.quoted)
                    throw new Diagnostic(value.location, "expected a skip pattern or literal");
                lexer.advance(); grammar.skip = value.text; grammar.skipPattern = value.kind == TokenKind.pattern;
                if (!value.text.length) throw new Diagnostic(value.location, "skip expression must not be empty");
                break;
            case "token":
                auto name = lexer.expectKind(TokenKind.identifier, "token name");
                auto value = lexer.current;
                if (value.kind != TokenKind.pattern && value.kind != TokenKind.quoted)
                    throw new Diagnostic(value.location, "expected a token pattern or literal");
                if (!value.text.length)
                    throw new Diagnostic(value.location, "token expression must not be empty");
                lexer.advance();
                grammar.tokens ~= TokenDefinition(name.text, value.text, value.kind == TokenKind.pattern, name.location);
                break;
            case "left": case "right": case "nonassoc":
                auto assoc = token.text == "left" ? Associativity.left : token.text == "right" ? Associativity.right : Associativity.nonassoc;
                Expression[] terminals;
                while (lexer.current.kind == TokenKind.identifier || lexer.current.kind == TokenKind.quoted) terminals ~= terminal();
                if (!terminals.length) throw new Diagnostic(token.location, "precedence declaration requires at least one terminal");
                grammar.precedences ~= PrecedenceDeclaration(assoc, terminals, token.location);
                break;
            case "rewrite": grammar.rewrites ~= lexer.expectKind(TokenKind.identifier, "rewrite name").text; break;
            default: throw new Diagnostic(token.location, "unknown directive %" ~ token.text);
        }
        lexer.expect(";");
    }

    Grammar read()
    {
        while (lexer.current.kind != TokenKind.end)
        {
            if (lexer.accept("%")) { directive(); continue; }
            auto name = lexer.expectKind(TokenKind.identifier, "rule name or directive");
            lexer.expect(":");
            Alternative[] alternatives;
            do
            {
                auto location = lexer.current.location;
                auto expression = sequence();
                Expression precedence;
                if (lexer.accept("%"))
                {
                    if (lexer.accept("empty"))
                    {
                        if (expression.children.length) throw new Diagnostic(location, "%empty must be the entire alternative");
                        if (lexer.accept("%")) { lexer.expect("prec"); precedence = terminal(); }
                    }
                    else { lexer.expect("prec"); precedence = terminal(); }
                }
                string label;
                if (lexer.accept("=>")) label = lexer.expectKind(TokenKind.identifier, "node label").text;
                alternatives ~= Alternative(expression, label, precedence, location);
            } while (lexer.accept("|"));
            lexer.expect(";");
            grammar.rules ~= Rule(name.text, alternatives, name.location);
        }
        return grammar;
    }
}

Grammar readGrammar(string source, string file = "<grammar>")
{
    return new GrammarReader(source, file).read();
}
