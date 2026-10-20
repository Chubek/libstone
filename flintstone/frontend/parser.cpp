#include "../internal.hpp"
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <locale>
#include <sstream>

namespace flintstone {
namespace {
struct Token {
    enum Kind { End, Name, Number, String, Punctuation } kind = End;
    std::string text;
    Location location;
};
class Parser {
public:
    Parser(std::string_view source, std::string filename, ParseLimits limits)
        : source_(source), filename_(std::move(filename)), limits_(limits) {
        detail::check(source.size() <= limits.bytes, "parse.limit", "source byte limit exceeded", {filename_, 1, 1});
        advance();
    }
    Module run() {
        Module module;
        expect("module"); module.name = name(); expect("level");
        if (take("high")) module.level = Level::High;
        else if (take("core")) module.level = Level::Core;
        else if (take("machine")) module.level = Level::Machine;
        else error("expected high, core or machine but found '" + token_.text + "'");
        expect("{");
        bool target_seen = false, attrs_seen = false;
        std::size_t declarations = 0;
        while (!take("}")) {
            detail::check(++declarations <= 200000, "parse.limit", "module declaration limit exceeded", token_.location);
            if (token_.kind == Token::End) error("unterminated module; expected '}'");
            if (take("target")) {
                if (target_seen) error("duplicate target declaration");
                target_seen = true;
                if (token_.kind != Token::String) error("expected target name string");
                module.target.name = token_.text; advance();
                auto values = attributes();
                auto width = [&](const std::string& key, unsigned& out) {
                    if (auto found = values.find(key); found != values.end()) {
                        auto number = std::get_if<std::int64_t>(&found->second);
                        if (!number || *number < 0 || std::uint64_t(*number) > UINT_MAX)
                            error("invalid target width for '" + key + "'");
                        out = static_cast<unsigned>(*number); values.erase(found);
                    }
                };
                width("pointer_bits", module.target.pointer_bits); width("index_bits", module.target.index_bits);
                if (auto endian = values.find("endian"); endian != values.end()) {
                    const auto* text = std::get_if<std::string>(&endian->second);
                    if (!text || (*text != "little" && *text != "big")) error("invalid target byte order");
                    module.target.endian = *text == "little" ? Endian::Little : Endian::Big;
                    values.erase(endian);
                }
                for (auto it = values.begin(); it != values.end();) {
                    if (it->first.rfind("feature.", 0) != 0) { ++it; continue; }
                    const auto* enabled = std::get_if<bool>(&it->second);
                    if (!enabled || !*enabled) error("target features must be true");
                    module.target.features.insert(it->first.substr(8)); it = values.erase(it);
                }
                module.target.parameters = std::move(values);
            } else if (take("import")) {
                if (token_.kind != Token::String) error("expected import library name string");
                auto lib = token_.text; advance(); expect(";");
                detail::check(!lib.empty(), "parse.import", "import library name is empty", token_.location);
                detail::check(module.imports.size() < 1000, "parse.limit", "import count limit exceeded", token_.location);
                for (const auto& existing : module.imports)
                    if (existing == lib) error("duplicate import '" + lib + "'");
                module.imports.push_back(std::move(lib));
            } else if (take("types")) {
                expect("{");
                while (!take("}")) {
                    detail::check(module.type_bindings.size() < 100000, "parse.limit", "type binding limit exceeded", token_.location);
                    auto key = name(); expect("="); auto value = type(); expect(";");
                    if (!module.type_bindings.emplace(key, std::move(value)).second) error("duplicate type binding '" + key + "'");
                }
            } else if (take("dialect")) {
                auto key = name(); auto values = attributes();
                if (!module.dialects.emplace(key, std::move(values)).second) error("duplicate dialect instance '" + key + "'");
            } else if (take("attrs")) {
                if (attrs_seen) error("duplicate module attributes");
                attrs_seen = true; module.attributes = attributes();
            } else {
                bool external = take("extern"); expect("func"); module.functions.push_back(function(external));
            }
        }
        if (token_.kind != Token::End) error("trailing input after module");
        return module;
    }
private:
    struct IDs {
        std::map<std::string, std::uint32_t> names;
        std::map<std::uint32_t, std::string> used;
        std::uint32_t next = 0;
    };
    [[noreturn]] void error(std::string text) const { detail::fail("parse", std::move(text), token_.location); }
    char peek(std::size_t n = 0) const {
        detail::check(position_ <= source_.size(), "parse.limit", "source position out of range", token_.location);
        return n < source_.size() - position_ ? source_[position_ + n] : '\0';
    }
    char consume() {
        detail::check(position_ < source_.size(), "parse.truncated", "unexpected end of input", token_.location);
        char c = source_[position_++];
        if (c == '\n') { ++line_; column_ = 1; } else ++column_;
        return c;
    }
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    static bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    void advance() {
        while (position_ < source_.size()) {
            if (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n') { consume(); continue; }
            if (peek() == '/' && peek(1) == '/') { while (position_ < source_.size() && peek() != '\n') consume(); continue; }
            break;
        }
        token_ = {}; token_.location = {filename_, line_, column_};
        detail::check(tokens_++ < limits_.tokens, "parse.limit", "token limit exceeded", token_.location);
        if (position_ == source_.size()) return;
        auto start = position_;
        char c = consume();
        if (alpha(c)) {
            token_.kind = Token::Name;
            while (alpha(peek()) || digit(peek()) || peek() == '.' || peek() == '-') {
                consume();
                detail::check(position_ - start <= 1024, "parse.limit", "identifier length limit exceeded", token_.location);
            }
        } else if (digit(c) || (c == '-' && digit(peek()))) {
            token_.kind = Token::Number;
            while (digit(peek())) consume();
            if (peek() == '.' && digit(peek(1))) { consume(); while (digit(peek())) consume(); }
            if (peek() == 'e' || peek() == 'E') {
                consume(); if (peek() == '+' || peek() == '-') consume();
                if (!digit(peek())) error("malformed exponent in '" + std::string(source_.substr(start, position_ - start)) + "'");
                while (digit(peek())) consume();
            }
            detail::check(position_ - start <= 1024, "parse.limit", "numeric literal length limit exceeded", token_.location);
        } else if (c == '"') {
            token_.kind = Token::String;
            while (position_ < source_.size() && peek() != '"') {
                char byte = consume();
                if (static_cast<unsigned char>(byte) < 32) error("unescaped control byte in string");
                if (byte == '\\') {
                    if (position_ == source_.size()) error("unterminated escape");
                    byte = consume();
                    if (byte == 'n') byte = '\n'; else if (byte == 'r') byte = '\r'; else if (byte == 't') byte = '\t';
                    else if (byte != '\\' && byte != '"') error(std::string("unsupported string escape '\\") + byte + "'");
                }
                token_.text += byte;
                detail::check(token_.text.size() <= 1024 * 1024, "parse.limit", "string literal length limit exceeded", token_.location);
            }
            if (position_ == source_.size()) error("unterminated string");
            consume(); return;
        } else {
            token_.kind = Token::Punctuation;
            if (c == '-' && peek() == '>') consume();
            else if (std::string_view("{}()[]<>,:;=%@^").find(c) == std::string_view::npos) {
                char hex[16];
                std::snprintf(hex, sizeof hex, "0x%02x", static_cast<unsigned char>(c));
                error(std::string("unexpected source byte ") + hex);
            }
        }
        token_.text = std::string(source_.substr(start, position_ - start));
    }
    bool take(std::string_view text) { if (token_.text != text || token_.kind == Token::String) return false; advance(); return true; }
    void expect(std::string_view text) {
        if (!take(text)) error("expected '" + std::string(text) + "' but found '" + token_.text + "'");
    }
    std::string name() {
        if (token_.kind != Token::Name) error("expected identifier but found '" + token_.text + "'");
        auto value = token_.text;
        advance();
        return value;
    }
    std::uint32_t unsigned_number(const std::string& text) {
        std::uint32_t result;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result >= invalid_id - 1)
            error("invalid unsigned ID/count '" + text + "'");
        return result;
    }
    std::uint32_t id(IDs& ids, const char* prefix) {
        expect(prefix);
        if (token_.kind != Token::Name && token_.kind != Token::Number) error("expected value/block ID");
        auto text = token_.text;
        if (auto found = ids.names.find(text); found != ids.names.end()) { advance(); return found->second; }
        std::uint32_t value;
        if (token_.kind == Token::Number) value = unsigned_number(text);
        else {
            while (ids.used.count(ids.next)) {
                if (ids.next >= invalid_id - 2) error("ID space exhausted");
                ++ids.next;
            }
            value = ids.next++;
        }
        if (ids.used.count(value)) error("numeric ID collides with a named ID; use one naming convention");
        ids.names[text] = value; ids.used[value] = text; advance(); return value;
    }
    Type type(std::size_t depth = 0) {
        if (depth >= limits_.type_depth || depth >= 64) error("type nesting limit exceeded");
        auto text = name();
        if (text == "void") return Type::void_type();
        if (text == "index") return Type::index();
        if (text.size() > 1 && (text[0] == 'i' || text[0] == 'f') && digit(text[1])) {
            auto bits = unsigned_number(text.substr(1));
            detail::check(bits >= 1, "parse.type", "scalar width must be at least 1", token_.location);
            return text[0] == 'i' ? Type::integer(bits) : Type::floating(bits);
        }
        expect("<");
        if (text == "ptr" || text == "param") {
            auto value = name(); expect(">"); return text == "ptr" ? Type::pointer(value) : Type::parameter(value);
        }
        if (text == "vector") {
            if (token_.kind != Token::Number) error("expected vector element count");
            auto count = unsigned_number(token_.text);
            detail::check(count >= 1, "parse.type", "vector count must be at least 1", token_.location);
            advance(); expect(","); auto element = type(depth + 1); expect(">");
            return Type::vector(count, std::move(element));
        }
        if (text == "tuple" || text == "opaque") {
            std::string opaque;
            std::vector<Type> elements;
            if (text == "opaque") { opaque = name(); if (take(">")) return Type::opaque(opaque); expect(","); }
            if (!take(">")) {
                do {
                    detail::check(elements.size() < 100000, "parse.limit", "tuple/opaque element limit exceeded", token_.location);
                    elements.push_back(type(depth + 1));
                } while (take(","));
                expect(">");
            }
            return text == "tuple" ? Type::tuple(std::move(elements)) : Type::opaque(std::move(opaque), std::move(elements));
        }
        error("unknown type constructor " + text);
    }
    Attribute atom() {
        if (token_.kind == Token::String) { auto text = token_.text; advance(); return text; }
        if (take("true")) return true;
        if (take("false")) return false;
        if (take("type")) { expect("<"); auto t = type(); expect(">"); return t; }
        if (token_.kind != Token::Number) error("expected attribute/literal value");
        auto text = token_.text;
        Attribute result;
        if (text.find_first_of(".eE") == std::string::npos) {
            std::int64_t number;
            auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) error("integer literal exceeds int64");
            result = number;
        } else {
            double number;
            auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(number)) error("invalid finite floating literal");
            result = number;
        }
        advance(); return result;
    }
    Attributes attributes() {
        Attributes result; expect("{");
        while (!take("}")) {
            detail::check(result.size() < 100000, "parse.limit", "attribute count limit exceeded", token_.location);
            auto key = name(); expect("="); auto value = atom(); expect(";");
            if (!result.emplace(key, std::move(value)).second) error("duplicate attribute '" + key + "'");
        }
        return result;
    }
    Operand operand(IDs& ids) {
        if (token_.text == "%") return Operand::ref(id(ids, "%"));
        if (take("@")) { auto symbol = name(); expect(":"); return Operand::named(std::move(symbol), type()); }
        auto value = atom(); expect(":"); auto t = type();
        if (auto integer = std::get_if<std::int64_t>(&value)) return Operand::constant(*integer, std::move(t));
        if (auto real = std::get_if<double>(&value)) return Operand::real(*real, std::move(t));
        error("operand literal must be numeric");
    }
    std::vector<BlockId> block_list(IDs& ids) {
        std::vector<BlockId> result; expect("[");
        if (!take("]")) {
            do {
                detail::check(result.size() < 100000, "parse.limit", "block label list limit exceeded", token_.location);
                result.push_back(id(ids, "^"));
            } while (take(","));
            expect("]");
        }
        return result;
    }
    Function function(bool external) {
        Function result; result.external = external; result.location = token_.location;
        expect("@"); result.name = name(); expect("("); IDs values, blocks;
        if (!take(")")) {
            do {
                detail::check(result.parameters.size() < 100000, "parse.limit", "parameter count limit exceeded", token_.location);
                auto loc = token_.location; auto value = id(values, "%"); expect(":"); result.parameters.push_back({value, type(), loc});
            } while (take(","));
            expect(")");
        }
        expect("->"); result.result = type();
        if (take("attrs")) result.attributes = attributes();
        if (external) { expect(";"); return result; }
        expect("{");
        if (take("entry")) { result.entry = id(blocks, "^"); expect(";"); }
        std::size_t total_ops = 0;
        while (!take("}")) {
            detail::check(result.blocks.size() < 10000, "parse.limit", "block count limit exceeded", token_.location);
            if (token_.kind == Token::End) error("unterminated function; expected '}'");
            Block block; block.location = token_.location; block.id = id(blocks, "^");
            if (result.entry == invalid_id) result.entry = block.id;
            if (token_.kind == Token::String) { block.name = token_.text; advance(); }
            expect(":");
            while (token_.text != "^" && token_.text != "}") {
                detail::check(++total_ops <= 1000000, "parse.limit", "operation count limit exceeded", token_.location);
                if (token_.kind == Token::End) error("unterminated block; expected operation or '}'");
                Operation op; op.location = token_.location;
                if (token_.text == "%") {
                    do {
                        detail::check(op.results.size() < 100000, "parse.limit", "result count limit exceeded", token_.location);
                        auto loc = token_.location; auto value = id(values, "%"); expect(":"); op.results.push_back({value, type(), loc});
                    } while (take(","));
                    expect("=");
                }
                op.name = name(); expect("(");
                if (!take(")")) {
                    do {
                        detail::check(op.operands.size() < 100000, "parse.limit", "operand count limit exceeded", token_.location);
                        op.operands.push_back(operand(values));
                    } while (take(","));
                    expect(")");
                }
                if (take("to")) op.successors = block_list(blocks);
                if (take("from")) op.incoming = block_list(blocks);
                if (take("attrs")) op.attributes = attributes();
                expect(";"); block.operations.push_back(std::move(op));
            }
            result.blocks.push_back(std::move(block));
        }
        return result;
    }
    std::string_view source_;
    std::string filename_;
    ParseLimits limits_;
    std::size_t position_ = 0, line_ = 1, column_ = 1, tokens_ = 0;
    Token token_;
};
std::string quote(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: detail::check(static_cast<unsigned char>(c) >= 32, "print.string", "unsupported control byte in string"); out += c;
        }
    }
    return out + '"';
}
std::string real_text(double value) {
    detail::check(std::isfinite(value), "print.float", "nonfinite floating value");
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17) << value;
    auto text = out.str(); if (text.find_first_of(".eE") == std::string::npos) text += ".0"; return text;
}
std::string attrs(const Attributes& attributes) {
    std::string out = "{ ";
    for (const auto& [key, value] : attributes) out += key + " = " + detail::attribute_text(value) + "; ";
    return out + "}";
}
std::string operand_text(const Operand& o) {
    if (o.kind == OperandKind::Value) return "%" + std::to_string(o.value);
    if (o.kind == OperandKind::Integer) return std::to_string(o.integer) + ":" + print(o.type);
    if (o.kind == OperandKind::Float) return real_text(o.floating) + ":" + print(o.type);
    return "@" + o.symbol + ":" + print(o.type);
}
}
namespace detail {
std::string attribute_text(const Attribute& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
    if (const auto* real = std::get_if<double>(&value)) return real_text(*real);
    if (const auto* boolean = std::get_if<bool>(&value)) return *boolean ? "true" : "false";
    if (const auto* text = std::get_if<std::string>(&value)) return quote(*text);
    if (const auto* type = std::get_if<Type>(&value)) return "type<" + print(*type) + ">";
    detail::fail("print.attribute", "invalid attribute variant");
}
}
Module parse(std::string_view source, std::string filename, const ParseLimits& limits) { return Parser(source, std::move(filename), limits).run(); }
std::string print(const Module& module) {
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "module " << module.name << " level " << level_name(module.level) << " {\n";
    Attributes target = module.target.parameters;
    target["pointer_bits"] = std::int64_t(module.target.pointer_bits); target["index_bits"] = std::int64_t(module.target.index_bits);
    target["endian"] = std::string(module.target.endian == Endian::Little ? "little" : "big");
    for (const auto& feature : module.target.features) target["feature." + feature] = true;
    out << "  target " << quote(module.target.name) << ' ' << attrs(target) << '\n';
    for (const auto& imported : module.imports) out << "  import " << quote(imported) << ";\n";
    if (!module.type_bindings.empty()) {
        out << "  types { "; for (const auto& [name, type] : module.type_bindings) out << name << " = " << print(type) << "; "; out << "}\n";
    }
    for (const auto& [name, parameters] : module.dialects) out << "  dialect " << name << ' ' << attrs(parameters) << '\n';
    if (!module.attributes.empty()) out << "  attrs " << attrs(module.attributes) << '\n';
    for (const auto& function : module.functions) {
        out << "  " << (function.external ? "extern " : "") << "func @" << function.name << '(';
        for (std::size_t n = 0; n < function.parameters.size(); ++n) {
            const auto& p = function.parameters[n]; if (n) out << ", "; out << '%' << p.id << ':' << print(p.type);
        }
        out << ") -> " << print(function.result);
        if (!function.attributes.empty()) out << " attrs " << attrs(function.attributes);
        if (function.external) { out << ";\n"; continue; }
        out << " {\n    entry ^" << function.entry << ";\n";
        for (const auto& block : function.blocks) {
            out << "    ^" << block.id;
            if (!block.name.empty()) out << ' ' << quote(block.name);
            out << ":\n";
            for (const auto& op : block.operations) {
                out << "      ";
                for (std::size_t n = 0; n < op.results.size(); ++n) {
                    if (n) out << ", ";
                    out << '%' << op.results[n].id << ':' << print(op.results[n].type);
                }
                if (!op.results.empty()) out << " = ";
                out << op.name << '(';
                for (std::size_t n = 0; n < op.operands.size(); ++n) { if (n) out << ", "; out << operand_text(op.operands[n]); }
                out << ')';
                auto labels = [&](const char* tag, const std::vector<BlockId>& ids) {
                    if (ids.empty()) return;
                    out << ' ' << tag << " [";
                    for (std::size_t n = 0; n < ids.size(); ++n) { if (n) out << ", "; out << '^' << ids[n]; }
                    out << ']';
                };
                labels("to", op.successors); labels("from", op.incoming);
                if (!op.attributes.empty()) out << " attrs " << attrs(op.attributes);
                out << ";\n";
            }
        }
        out << "  }\n";
    }
    out << "}\n"; return out.str();
}
} // namespace flintstone
