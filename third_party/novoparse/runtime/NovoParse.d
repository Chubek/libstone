module NovoParse;

import std.json : JSONValue, parseJSON;
import std.string : fromStringz;

struct SymbolSpec
{
    int kind;
    const(char)* name;
    const(char)* value;
    int precedence;
    int associativity;
}

struct ProductionSpec
{
    int head;
    const(int)* body;
    size_t body_length;
    const(char)* label;
    int precedence;
    int associativity;
}

struct GrammarSpec
{
    const(char)* name;
    const(SymbolSpec)* symbols;
    size_t symbol_count;
    const(ProductionSpec)* productions;
    size_t production_count;
    int start;
    int skip_kind;
    const(char)* skip;
    int disambiguation;
    const(char)* rewrites;
}

extern(C)
{
    void* np_parser_create(const(GrammarSpec)* spec, char* error, size_t error_size);
    void np_parser_destroy(void* parser);
    int np_parser_parse(void* parser, const(char)* input, size_t length);
    const(char)* np_parser_json(void* parser);
    const(char)* np_parser_error(const(void)* parser);
    size_t np_parser_position(const(void)* parser);
    size_t np_parser_line(const(void)* parser);
    size_t np_parser_column(const(void)* parser);
    int np_parser_ambiguous(const(void)* parser);
    size_t np_parser_conflicts(const(void)* parser);
}

class ParseError : Exception
{
    int status;
    size_t position, line, column;
    this(void* parser, int status)
    {
        super(fromStringz(np_parser_error(parser)).idup);
        this.status = status;
        position = np_parser_position(parser);
        line = np_parser_line(parser);
        column = np_parser_column(parser);
    }
}

class Parser
{
    private void* handle;

    this(const(GrammarSpec)* spec)
    {
        char[512] error;
        handle = np_parser_create(spec, error.ptr, error.length);
        if (handle is null) throw new Exception(fromStringz(error.ptr).idup);
    }

    ~this() { close(); }

    void close()
    {
        if (handle !is null) { np_parser_destroy(handle); handle = null; }
    }

    private void requireOpen() const
    {
        if (handle is null) throw new Exception("parser is closed");
    }

    JSONValue parse(string input)
    {
        requireOpen();
        auto status = np_parser_parse(handle, input.length ? input.ptr : "".ptr, input.length);
        if (status) throw new ParseError(handle, status);
        auto text = np_parser_json(handle);
        if (text is null) throw new Exception(fromStringz(np_parser_error(handle)).idup);
        return parseJSON(fromStringz(text).idup);
    }

    bool ambiguous() const { requireOpen(); return np_parser_ambiguous(handle) != 0; }
    size_t conflicts() const { requireOpen(); return np_parser_conflicts(handle); }
}
