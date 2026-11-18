#include "WITParser.hpp"

#include <set>
#include <utility>

namespace limestone::wasmutils {
namespace {

constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool lower(char c) { return c >= 'a' && c <= 'z'; }
constexpr bool upper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool letter(char c) { return lower(c) || upper(c); }
constexpr char fold(char c) { return upper(c) ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool valid_name(std::string_view name) {
    if (name.empty() || !letter(name.front()))
        return false;
    bool seen_lower = false;
    bool seen_upper = false;
    bool nonempty = false;
    for (char c : name) {
        if (c == '-') {
            if (!nonempty)
                return false;
            seen_lower = seen_upper = nonempty = false;
        } else {
            if (!(letter(c) || digit(c)))
                return false;
            nonempty = true;
            seen_lower |= lower(c);
            seen_upper |= upper(c);
            if (seen_lower && seen_upper)
                return false;
        }
    }
    return nonempty;
}

bool valid_version(std::string_view text) {
    for (int part = 0; part < 3; ++part) {
        std::size_t length = 0;
        while (length < text.size() && digit(text[length]))
            ++length;
        if (length == 0 || (length > 1 && text.front() == '0'))
            return false;
        text.remove_prefix(length);
        if (part < 2) {
            if (text.empty() || text.front() != '.')
                return false;
            text.remove_prefix(1);
        }
    }
    return text.empty();
}

struct ScalarMapping {
    WitScalar kind;
    std::string_view wit;
    std::string_view c;
};

constexpr ScalarMapping scalars[] = {
    {WitScalar::Bool, "bool", "int"},
    {WitScalar::U8, "u8", "unsigned char"},
    {WitScalar::S8, "s8", "signed char"},
    {WitScalar::U16, "u16", "wit_host_u16"},
    {WitScalar::S16, "s16", "wit_host_s16"},
    {WitScalar::U32, "u32", "wit_host_u32"},
    {WitScalar::S32, "s32", "wit_host_s32"},
    {WitScalar::U64, "u64", "wit_host_u64"},
    {WitScalar::S64, "s64", "wit_host_s64"},
    {WitScalar::F32, "f32", "float"},
    {WitScalar::F64, "f64", "double"},
    {WitScalar::Char, "char", "wit_host_u32"},
};

bool is_keyword(std::string_view word) {
    constexpr std::string_view keywords[] = {
        "as", "async", "borrow", "constructor", "enum", "export", "flags",
        "from", "func", "future", "import", "include", "interface", "list",
        "map", "option", "own", "package", "record", "resource", "result",
        "static", "stream", "string", "tuple", "type", "use", "variant",
        "with", "world",
    };
    for (auto keyword : keywords)
        if (word == keyword)
            return true;
    for (const auto& scalar : scalars)
        if (word == scalar.wit)
            return true;
    return false;
}

struct Token {
    std::string_view text;
    WitLocation location;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        skip();
        const WitLocation start = location_;
        if (at_end())
            return {{}, start};

        const char c = peek();
        if (letter(c) || c == '%') {
            // Leave '->' as a single token even when adjacent to a type.
            advance();
            while (!at_end() &&
                   (letter(peek()) || digit(peek()) || (peek() == '-' && !starts("->"))))
                advance();
        } else if (digit(c)) {
            // One version token, so whitespace and comments inside it are
            // rejected by valid_version instead of accidentally normalized.
            while (!at_end() && (digit(peek()) || peek() == '.'))
                advance();
        } else if (starts("->")) {
            advance();
            advance();
        } else {
            advance();
        }
        return {source_.substr(start.offset, location_.offset - start.offset), start};
    }

private:
    std::string_view source_;
    WitLocation location_;

    bool at_end() const { return location_.offset == source_.size(); }
    char peek() const { return source_[location_.offset]; }
    bool starts(std::string_view prefix) const {
        return source_.substr(location_.offset).starts_with(prefix);
    }

    void advance() {
        const char c = peek();
        if (!space(c) && (c < '!' || c > '~'))
            throw WitError(location_, "expected printable ASCII or WIT whitespace");
        ++location_.offset;
        if (c == '\r' || (c == '\n' &&
                          (location_.offset == 1 || source_[location_.offset - 2] != '\r'))) {
            ++location_.line;
            location_.column = 1;
        } else if (c != '\n') {
            ++location_.column;
        }
    }

    void skip() {
        while (!at_end()) {
            if (space(peek())) {
                advance();
            } else if (starts("//")) {
                while (!at_end() && peek() != '\n')
                    advance();
            } else if (starts("/*")) {
                skip_block_comment();
            } else {
                break;
            }
        }
    }

    void skip_block_comment() {
        const auto start = location_;
        std::size_t depth = 1;
        advance();
        advance();
        while (depth != 0) {
            if (at_end())
                throw WitError(start, "unterminated block comment");
            if (starts("/*")) {
                ++depth;
                advance();
                advance();
            } else if (starts("*/")) {
                --depth;
                advance();
                advance();
            } else {
                advance();
            }
        }
    }
};

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source), token_(lexer_.next()) {}

    WitDocument parse() {
        WitDocument document;
        if (take("package"))
            document.package = package();
        do {
            if (!take("interface"))
                fail("expected an interface declaration; only standalone interfaces are supported");
            document.interfaces.push_back(interface_decl());
        } while (!token_.text.empty());
        return document;
    }

private:
    Lexer lexer_;
    Token token_;

    [[noreturn]] void fail(const std::string& message) const {
        throw WitError(token_.location, message);
    }

    bool take(std::string_view text) {
        if (token_.text != text)
            return false;
        token_ = lexer_.next();
        return true;
    }

    void expect(std::string_view text) {
        if (!take(text))
            fail("expected '" + std::string(text) + "'");
    }

    WitName name() {
        auto text = token_.text;
        const bool escaped = text.starts_with('%');
        if (escaped)
            text.remove_prefix(1);
        if (!valid_name(text))
            fail("expected a kebab-case WIT identifier");
        if (!escaped && is_keyword(text))
            fail("unexpected WIT keyword where a name was expected; only scalar functions are supported in interfaces (escape a keyword name with '%')");
        WitName result{std::string(text), token_.location};
        token_ = lexer_.next();
        return result;
    }

    WitPackage package() {
        auto name_space = name();
        expect(":");
        WitPackage result{std::move(name_space), name(), std::nullopt};
        if (take("@")) {
            if (!valid_version(token_.text))
                fail("expected a package version of the form major.minor.patch");
            result.version = WitVersion{std::string(token_.text), token_.location};
            token_ = lexer_.next();
            if (token_.text == "-" || token_.text == "+")
                fail("package prerelease and build suffixes are not supported");
        }
        expect(";");
        return result;
    }

    WitInterface interface_decl() {
        WitInterface result{name(), {}};
        expect("{");
        while (!take("}")) {
            if (token_.text.empty())
                fail("expected a function declaration or '}'");
            result.functions.push_back(function());
        }
        return result;
    }

    WitFunction function() {
        WitFunction result{name(), {}, std::nullopt};
        expect(":");
        if (!take("func"))
            fail("expected 'func'; only synchronous function declarations are supported");
        expect("(");
        if (!take(")")) {
            for (;;) {
                auto parameter_name = name();
                expect(":");
                result.parameters.push_back({std::move(parameter_name), type()});
                if (!take(",")) {
                    expect(")");
                    break;
                }
                if (take(")"))
                    break;
            }
        }
        if (take("->"))
            result.result = type();
        expect(";");
        return result;
    }

    WitType type() {
        for (const auto& scalar : scalars) {
            if (scalar.wit == token_.text) {
                const WitType result{scalar.kind, token_.location};
                token_ = lexer_.next();
                return result;
            }
        }
        fail("expected a supported scalar type (bool, u8/s8, u16/s16, u32/s32,"
             " u64/s64, f32/f64, or char)");
    }
};

void check_name(const WitName& name) {
    if (!valid_name(name.text))
        throw WitError(name.location, "invalid WIT identifier in AST");
}

void check_unique(std::set<std::string>& names, const WitName& name,
                  std::string_view scope) {
    check_name(name);
    std::string key;
    for (char c : name.text)
        if (c != '-')
            key += fold(c);
    if (!names.insert(std::move(key)).second)
        throw WitError(name.location, "duplicate " + std::string(scope) +
                       " name '" + name.text + "'");
}

std::string_view c_type(const WitType& type) {
    for (const auto& scalar : scalars)
        if (scalar.kind == type.scalar)
            return scalar.c;
    throw WitError(type.location, "invalid WIT scalar type in AST");
}

void validate(const WitDocument& document) {
    if (document.interfaces.empty())
        throw WitError({}, "expected at least one interface");
    if (const auto& package = document.package) {
        check_name(package->name_space);
        check_name(package->name);
        if (package->version && !valid_version(package->version->text))
            throw WitError(package->version->location, "invalid WIT package version in AST");
    }
    std::set<std::string> interface_names;
    for (const auto& interface : document.interfaces) {
        check_unique(interface_names, interface.name, "interface");
        std::set<std::string> function_names;
        for (const auto& function : interface.functions) {
            check_unique(function_names, function.name, "function");
            std::set<std::string> parameter_names;
            for (const auto& parameter : function.parameters) {
                check_unique(parameter_names, parameter.name, "parameter");
                c_type(parameter.type);
            }
            if (function.result)
                c_type(*function.result);
        }
    }
}

void append_label(std::string& output, const WitName& name) {
    for (char c : name.text)
        output += (c == '-' ? '_' : fold(c));
}

constexpr std::string_view c_prelude = R"C(/* Generated by wit2c: ANSI C host declarations only.
 * Not a WebAssembly Component Model canonical ABI or a wire format.
 * WIT bool uses int (0 or 1); char is a Unicode scalar value in wit_host_u32.
 * f32/f64 use native float/double, without an IEEE encoding or NaN guarantee.
 * u64/s64 use high and low 32-bit limbs. For u64 the value is high * 2^32
 * plus low; for s64 the same limbs encode the two's complement bit pattern.
 * Struct padding and calling conventions are those of the host C compiler.
 */

#ifndef WIT_HOST_SCALARS_DEFINED
#define WIT_HOST_SCALARS_DEFINED
#include <limits.h>

#if CHAR_BIT != 8 || UCHAR_MAX != 255 || SCHAR_MIN >= -127 || SCHAR_MAX != 127
#error "wit2c requires 8-bit char with two's complement signed char"
#endif

#if USHRT_MAX == 65535U && SHRT_MIN < -32767 && SHRT_MAX == 32767
typedef unsigned short wit_host_u16;
typedef signed short wit_host_s16;
#elif UINT_MAX == 65535U && INT_MIN < -32767 && INT_MAX == 32767
typedef unsigned int wit_host_u16;
typedef signed int wit_host_s16;
#else
#error "wit2c requires 16-bit short or int with two's complement signed range"
#endif

#if UINT_MAX == 4294967295UL && INT_MIN < -2147483647L && INT_MAX == 2147483647L
typedef unsigned int wit_host_u32;
typedef signed int wit_host_s32;
#elif ULONG_MAX == 4294967295UL && LONG_MIN < -2147483647L && LONG_MAX == 2147483647L
typedef unsigned long wit_host_u32;
typedef signed long wit_host_s32;
#else
#error "wit2c requires 32-bit int or long with two's complement signed range"
#endif

typedef struct wit_host_u64 {
    wit_host_u32 high;
    wit_host_u32 low;
} wit_host_u64;

typedef struct wit_host_s64 {
    wit_host_u32 high;
    wit_host_u32 low;
} wit_host_s64;

#endif /* WIT_HOST_SCALARS_DEFINED */

)C";

} // namespace

WitDocument parse_wit(std::string_view source) {
    auto document = Parser(source).parse();
    validate(document);
    return document;
}

std::string emit_wit_c(const WitDocument& document) {
    validate(document);
    std::string output(c_prelude);
    output += "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";

    // "_W_" cannot appear inside a folded label, so qualifying names by
    // concatenation cannot cause collisions even across multiple headers.
    std::string prefix = "wit_";
    if (const auto& package = document.package) {
        append_label(prefix, package->name_space);
        prefix += "_W_";
        append_label(prefix, package->name);
        prefix += "_W_";
        if (package->version) {
            prefix += 'v';
            for (char c : package->version->text)
                prefix += c == '.' ? 'v' : c;
            prefix += "_W_";
        }
    }
    for (const auto& interface : document.interfaces) {
        std::string qualified = prefix;
        append_label(qualified, interface.name);
        qualified += "_W_";
        for (const auto& function : interface.functions) {
            output += function.result ? c_type(*function.result) : "void";
            output += '\n';
            output += qualified;
            append_label(output, function.name);
            output += '(';
            if (function.parameters.empty())
                output += "void";
            for (std::size_t i = 0; i < function.parameters.size(); ++i) {
                if (i != 0)
                    output += ",\n    ";
                const auto& parameter = function.parameters[i];
                output += c_type(parameter.type);
                output += " wit_arg_";
                append_label(output, parameter.name);
            }
            output += ");\n";
        }
    }
    output += "\n#ifdef __cplusplus\n}\n#endif\n";
    return output;
}

std::string wit_to_c(std::string_view source) {
    return emit_wit_c(parse_wit(source));
}

} // namespace limestone::wasmutils
