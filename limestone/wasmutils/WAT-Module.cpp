#include "WAT-Module.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <map>
#include <utility>

namespace limestone::wasmutils::core {
namespace {

constexpr unsigned nesting_limit = 128;

bool space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

int digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool fits_digit(char ch, unsigned base) {
    const auto d = digit(ch);
    return d >= 0 && static_cast<unsigned>(d) < base;
}

bool idchar(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           std::string_view("!#$%&'*+-./:<=>?@\\^_`|~").find(ch) != std::string_view::npos;
}

// Text uses UTF-8, but string escapes may denote arbitrary bytes. Names still
// require valid UTF-8 after escape expansion.
unsigned utf8_width(std::string_view text) {
    const auto first = static_cast<unsigned char>(text[0]);
    if (first < 0x80) return 1;
    const unsigned width = first < 0xc2 ? 0 : first < 0xe0 ? 2 :
                           first < 0xf0 ? 3 : first < 0xf5 ? 4 : 0;
    if (!width || text.size() < width) return 0;
    const auto second = static_cast<unsigned char>(text[1]);
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
        (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90))
        return 0;
    for (unsigned i = 1; i < width; ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) return 0;
    return width;
}

std::uint64_t digits(std::string_view text, unsigned base, std::uint64_t limit,
                     std::size_t offset, std::string_view what) {
    std::uint64_t value = 0;
    bool need_digit = true;
    for (const auto ch : text) {
        if (ch == '_' && !need_digit) {
            need_digit = true;
            continue;
        }
        if (!fits_digit(ch, base))
            throw Error(offset, "invalid " + std::string(what));
        const auto d = static_cast<unsigned>(digit(ch));
        if (value > limit / base || (value == limit / base && d > limit % base))
            throw Error(offset, std::string(what) + " out of range");
        value = value * base + d;
        need_digit = false;
    }
    if (need_digit)
        throw Error(offset, "invalid " + std::string(what));
    return value;
}

void append_utf8(std::string& result, std::uint32_t value) {
    if (value < 0x80) {
        result += static_cast<char>(value);
        return;
    }
    const unsigned width = value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
    const unsigned prefix = (0xffu << (8 - width)) & 0xffu;
    result += static_cast<char>(prefix | (value >> (6 * (width - 1))));
    for (unsigned i = width - 1; i > 0; --i)
        result += static_cast<char>(0x80u | ((value >> (6 * (i - 1))) & 0x3fu));
}

struct Node {
    enum Kind { atom, string, list } kind;
    std::size_t offset;
    std::string_view text;
    std::string bytes;
    std::vector<Node> children;
    std::size_t end{}; // Byte offset of closing parenthesis (list only).

    bool is(std::string_view token) const { return kind == atom && text == token; }
    bool form(std::string_view name) const {
        return kind == list && !children.empty() && children.front().is(name);
    }
};

class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}

    Node document() {
        skip();
        const auto start = cursor_;
        Node result{Node::list, start, {}, {}, {{Node::atom, start, "module", {}, {}}}};
        // The outer (module ...) wrapper can be omitted in a source file.
        while (cursor_ != input_.size()) {
            auto field = node(1);
            skip();
            if (result.children.size() == 1 && field.form("module")) {
                if (cursor_ != input_.size()) throw Error(cursor_, "unexpected text after module");
                return field;
            }
            result.children.push_back(std::move(field));
        }
        result.end = cursor_;
        return result;
    }

private:
    std::string_view input_;
    std::size_t cursor_{};

    std::string_view remaining() const { return input_.substr(cursor_); }

    void comment_character() {
        const auto width = utf8_width(remaining());
        if (!width) throw Error(cursor_, "invalid UTF-8 in comment");
        cursor_ += width;
    }

    void skip() {
        while (cursor_ < input_.size()) {
            if (space(input_[cursor_])) {
                ++cursor_;
            } else if (remaining().starts_with(";;")) {
                cursor_ += 2;
                while (cursor_ < input_.size() && input_[cursor_] != '\r' && input_[cursor_] != '\n')
                    comment_character();
            } else if (remaining().starts_with("(;")) {
                const auto start = cursor_;
                cursor_ += 2;
                unsigned depth = 1;
                while (cursor_ < input_.size() && depth) {
                    if (remaining().starts_with("(;")) {
                        if (++depth > nesting_limit)
                            throw Error(cursor_, "block comment nesting limit exceeded");
                        cursor_ += 2;
                    } else if (remaining().starts_with(";)")) {
                        --depth;
                        cursor_ += 2;
                    } else {
                        comment_character();
                    }
                }
                if (depth) throw Error(start, "unterminated block comment");
            } else {
                break;
            }
        }
    }

    Node node(unsigned depth) {
        const auto start = cursor_;
        const auto ch = input_[cursor_++];
        if (ch == '(') {
            if (depth == nesting_limit) throw Error(start, "WAT nesting limit exceeded");
            Node result{Node::list, start, {}, {}, {}};
            for (;;) {
                skip();
                if (cursor_ == input_.size()) throw Error(start, "unterminated parenthesized form");
                if (input_[cursor_] == ')') {
                    result.end = cursor_++;
                    return result;
                }
                result.children.push_back(node(depth + 1));
            }
        }
        if (ch == '"') {
            auto bytes = string(start);
            if (cursor_ < input_.size() && (idchar(input_[cursor_]) || input_[cursor_] == '"'))
                throw Error(start, "expected a separator after string");
            return {Node::string, start, {}, std::move(bytes), {}};
        }
        if (ch == ')') throw Error(start, "unexpected ')'");
        if (!idchar(ch)) throw Error(start, "invalid token in WAT source");
        while (cursor_ < input_.size() && idchar(input_[cursor_]))
            ++cursor_;
        if (cursor_ < input_.size() && input_[cursor_] == '"')
            throw Error(start, "expected a separator before string");
        return {Node::atom, start, input_.substr(start, cursor_ - start), {}, {}};
    }

    std::string string(std::size_t start) {
        std::string result;
        while (cursor_ < input_.size()) {
            const auto offset = cursor_;
            const auto ch = input_[cursor_++];
            if (ch == '"') return result;
            if (ch != '\\') {
                const auto code = static_cast<unsigned char>(ch);
                if (code < 0x20 || code == 0x7f)
                    throw Error(offset, "unescaped control character in string");
                const auto width = utf8_width(input_.substr(offset));
                if (!width) throw Error(offset, "invalid UTF-8 in string");
                result.append(input_.substr(offset, width));
                cursor_ = offset + width;
                continue;
            }
            if (cursor_ == input_.size()) break;
            const auto escape = input_[cursor_++];
            switch (escape) {
            case 't': result += '\t'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case '"': case '\'': case '\\': result += escape; break;
            case 'u': {
                if (!remaining().starts_with('{')) throw Error(offset, "expected '{' after \\u");
                const auto end = input_.find('}', ++cursor_);
                if (end == std::string_view::npos) throw Error(offset, "unterminated Unicode escape");
                const auto value = digits(input_.substr(cursor_, end - cursor_), 16, 0x10ffff,
                                          offset, "Unicode escape");
                if (value >= 0xd800 && value <= 0xdfff)
                    throw Error(offset, "Unicode escape is not a scalar value");
                append_utf8(result, static_cast<std::uint32_t>(value));
                cursor_ = end + 1;
                break;
            }
            default:
                if (!fits_digit(escape, 16) || cursor_ == input_.size() ||
                    !fits_digit(input_[cursor_], 16))
                    throw Error(offset, "invalid string escape");
                result += static_cast<char>((digit(escape) << 4) | digit(input_[cursor_++]));
                break;
            }
        }
        throw Error(start, "unterminated string");
    }
};

class Cursor {
public:
    explicit Cursor(const Node& form, std::size_t first = 1)
        : nodes_(form.children), end_(form.end), next_(first) {}

    const Node* peek() const { return next_ < nodes_.size() ? &nodes_[next_] : nullptr; }
    bool empty() const { return !peek(); }
    std::size_t offset() const { return empty() ? end_ : peek()->offset; }
    bool is(std::string_view token) const { return !empty() && peek()->is(token); }
    bool form(std::string_view name) const { return !empty() && peek()->form(name); }
    const Node& take(std::string_view what) {
        if (empty()) throw Error(end_, "expected " + std::string(what));
        return nodes_[next_++];
    }
    void done(std::string_view context) const {
        if (!empty()) throw Error(offset(), "unexpected text in " + std::string(context));
    }

private:
    std::span<const Node> nodes_;
    std::size_t end_;
    std::size_t next_;
};

std::string_view atom(const Node& n, std::string_view what) {
    if (n.kind != Node::atom) throw Error(n.offset, "expected " + std::string(what));
    return n.text;
}

bool identifier(const Node* n) {
    return n && n->kind == Node::atom && n->text.starts_with('$');
}

const Node* optional_id(Cursor& c) {
    if (!identifier(c.peek())) return nullptr;
    const auto& n = c.take("identifier");
    if (n.text.size() == 1) throw Error(n.offset, "empty WAT identifier");
    return &n;
}

const std::string& string(const Node& n) {
    if (n.kind != Node::string) throw Error(n.offset, "expected a quoted string");
    return n.bytes;
}

std::string name(const Node& n) {
    const auto& bytes = string(n);
    auto text = std::string_view(bytes);
    while (!text.empty()) {
        const auto width = utf8_width(text);
        if (!width) throw Error(n.offset, "name is not valid UTF-8");
        text.remove_prefix(width);
    }
    return bytes;
}

std::uint64_t natural(std::string_view text, std::uint64_t limit, std::size_t offset,
                      std::string_view what) {
    if (text.starts_with("0x")) return digits(text.substr(2), 16, limit, offset, what);
    return digits(text, 10, limit, offset, what);
}

std::uint32_t u32(const Node& n, std::string_view what) {
    return static_cast<std::uint32_t>(natural(atom(n, what), UINT32_MAX, n.offset, what));
}

std::uint64_t integer(const Node& n, unsigned width) {
    auto text = atom(n, "integer constant");
    const bool negative = text.starts_with('-');
    const bool positive = text.starts_with('+');
    if (negative || positive) text.remove_prefix(1);
    const auto mask = UINT64_MAX >> (64 - width);
    const auto limit = negative ? std::uint64_t{1} << (width - 1) : positive ? mask / 2 : mask;
    const auto magnitude = natural(text, limit, n.offset, "integer constant");
    return negative ? (std::uint64_t{0} - magnitude) & mask : magnitude;
}

template<class Float, class UInt, unsigned Mantissa>
UInt floating(const Node& n) {
    auto text = atom(n, "floating-point constant");
    constexpr UInt sign = UInt{1} << (sizeof(UInt) * 8 - 1);
    constexpr UInt mantissa = (UInt{1} << Mantissa) - 1;
    constexpr UInt infinity = (sign - 1) & ~mantissa;
    static_assert(std::numeric_limits<Float>::is_iec559 && sizeof(Float) == sizeof(UInt));
    const UInt rawsign = text.starts_with('-') ? sign : 0;
    if (rawsign || text.starts_with('+')) text.remove_prefix(1);
    if (text == "inf") return rawsign | infinity;
    if (text == "nan") return rawsign | infinity | (UInt{1} << (Mantissa - 1));
    if (text.starts_with("nan:0x")) {
        const auto payload = static_cast<UInt>(digits(text.substr(6), 16, mantissa,
                                                      n.offset, "NaN payload"));
        if (!payload) throw Error(n.offset, "NaN payload must be nonzero");
        return rawsign | infinity | payload;
    }

    const bool hex = text.starts_with("0x");
    if (hex) text.remove_prefix(2);
    const unsigned base = hex ? 16 : 10;
    std::string literal;
    literal.reserve(text.size());
    std::size_t cursor = 0;
    std::size_t count = 0, first_nonzero = std::string_view::npos;
    const auto scan_digits = [&](unsigned radix, bool significand) {
        bool any = false;
        while (cursor < text.size() && fits_digit(text[cursor], radix)) {
            const auto ch = text[cursor++];
            literal += ch;
            if (significand) {
                if (ch != '0' && first_nonzero == std::string_view::npos)
                    first_nonzero = count;
                ++count;
            }
            any = true;
            if (cursor < text.size() && text[cursor] == '_') {
                ++cursor;
                if (cursor == text.size() || !fits_digit(text[cursor], radix))
                    throw Error(n.offset, "invalid digit separator in floating-point constant");
            }
        }
        return any;
    };
    if (!scan_digits(base, true))
        throw Error(n.offset, "expected floating-point constant");
    const auto integral = count;
    if (cursor < text.size() && text[cursor] == '.') {
        literal += text[cursor++];
        scan_digits(base, true);
    }
    long double exponent = 0;
    if (cursor < text.size() &&
        (hex ? text[cursor] == 'p' || text[cursor] == 'P'
             : text[cursor] == 'e' || text[cursor] == 'E')) {
        literal += text[cursor++];
        bool neg_exp = false;
        if (cursor < text.size() && (text[cursor] == '-' || text[cursor] == '+')) {
            neg_exp = text[cursor] == '-';
            literal += text[cursor++];
        }
        const auto begin = literal.size();
        if (!scan_digits(10, false)) throw Error(n.offset, "missing floating-point exponent");
        // Clamp to a number larger than the significand could possibly offset.
        const auto cap = 4 * static_cast<long double>(text.size()) + 1024;
        for (std::size_t i = begin; i < literal.size() && exponent < cap; ++i)
            exponent = exponent * 10 + digit(literal[i]);
        if (neg_exp) exponent = -exponent;
    }
    if (cursor != text.size()) throw Error(n.offset, "invalid floating-point constant");
    if (first_nonzero == std::string_view::npos) return rawsign;

    Float value{};
    const auto last = literal.data() + literal.size();
    const auto parsed = std::from_chars(literal.data(), last, value,
                                       hex ? std::chars_format::hex : std::chars_format::general);
    if (parsed.ptr != last) throw Error(n.offset, "invalid floating-point constant");
    if (parsed.ec == std::errc::result_out_of_range) {
        // from_chars also reports underflow when rounding to zero. Distinguish
        // that from overflow without using the host locale or losing subnormals.
        const auto order = static_cast<long double>(integral) -
                           static_cast<long double>(first_nonzero) - 1;
        if (exponent + order * (hex ? 4 : 1) < 0) return rawsign;
        throw Error(n.offset, "floating-point constant overflows");
    }
    if (parsed.ec != std::errc{}) throw Error(n.offset, "invalid floating-point constant");
    return rawsign | std::bit_cast<UInt>(value);
}

Type value_type(const Node& n) {
    const auto text = atom(n, "value type");
    if (text == "i32") return Type::i32;
    if (text == "i64") return Type::i64;
    if (text == "f32") return Type::f32;
    if (text == "f64") return Type::f64;
    if (text == "v128") return Type::v128;
    if (text == "funcref") return Type::funcref;
    if (text == "externref") return Type::externref;
    throw Error(n.offset, "unknown Core 2.0 value type");
}

Type ref_type(const Node& n) {
    if (n.is("funcref")) return Type::funcref;
    if (n.is("externref")) return Type::externref;
    throw Error(n.offset, "expected funcref or externref");
}

bool index_token(const Node* n) {
    return identifier(n) || (n && n->kind == Node::atom && !n->text.empty() &&
                            fits_digit(n->text.front(), 10));
}

std::uint32_t count(std::size_t value, std::size_t offset) {
    if (value > UINT32_MAX) throw Error(offset, "too many WAT declarations");
    return static_cast<std::uint32_t>(value);
}

struct IndexSpace {
    std::string_view kind;
    std::uint32_t size{};
    std::map<std::string_view, std::uint32_t> names;

    std::uint32_t add(const Node* id, std::size_t offset) {
        if (size == UINT32_MAX) throw Error(offset, "too many " + std::string(kind) + " declarations");
        if (id && !names.emplace(id->text, size).second)
            throw Error(id->offset, "duplicate " + std::string(kind) + " identifier");
        return size++;
    }

    std::uint32_t check(std::uint32_t index, std::size_t offset) const {
        if (index >= size)
            throw Error(offset, std::string(kind) + " index out of range");
        return index;
    }

    std::uint32_t resolve(const Node& n) const {
        if (identifier(&n)) {
            if (const auto found = names.find(n.text); found != names.end())
                return found->second;
            throw Error(n.offset, "unknown " + std::string(kind) + " identifier");
        }
        return check(u32(n, std::string(kind) + " index"), n.offset);
    }

    std::uint32_t resolve(Cursor& c) const {
        return resolve(c.take(std::string(kind) + " index"));
    }

    std::uint32_t optional(Cursor& c, std::size_t offset) const {
        return index_token(c.peek()) ? resolve(c) : check(0, offset);
    }
};

void types(Cursor& c, std::vector<Type>& result) {
    while (!c.empty()) result.push_back(value_type(c.take("value type")));
}

struct Signature {
    FunctionType type;
    const Node* type_use{};
    std::vector<const Node*> parameter_names;
};

Signature signature(Cursor& c, bool named_params, bool explicit_type = true) {
    Signature sig;
    if (explicit_type && c.form("type")) {
        Cursor use(c.take("type use"));
        sig.type_use = &use.take("type index");
        use.done("type use");
    }
    while (c.form("param")) {
        Cursor param(c.take("parameter declaration"));
        const auto* id = optional_id(param);
        if (id) {
            if (!named_params)
                throw Error(id->offset, "parameter names are not allowed in this type use");
            sig.type.params.push_back(value_type(param.take("parameter type")));
            sig.parameter_names.push_back(id);
            param.done("named parameter declaration");
        } else {
            types(param, sig.type.params);
            sig.parameter_names.resize(sig.type.params.size(), nullptr);
        }
    }
    while (c.form("result")) {
        Cursor result(c.take("result declaration"));
        types(result, sig.type.results);
    }
    return sig;
}

Limits limits(Cursor& c) {
    Limits result{u32(c.take("minimum limit"), "minimum limit"), {}};
    if (index_token(c.peek()) && !identifier(c.peek()))
        result.max = u32(c.take("maximum limit"), "maximum limit");
    return result;
}

GlobalType global_type(const Node& n) {
    if (!n.form("mut")) return {value_type(n), false};
    Cursor mut(n);
    const auto type = value_type(mut.take("mutable global type"));
    mut.done("mutable global type");
    return {type, true};
}

std::vector<std::uint8_t> data_bytes(Cursor& c) {
    std::vector<std::uint8_t> result;
    while (!c.empty()) {
        const auto& bytes = string(c.take("data string"));
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    return result;
}

enum class FieldKind { type, function, table, memory, global, elem, data, start, export_ };

std::optional<FieldKind> external(const Node& n) {
    if (n.form("func")) return FieldKind::function;
    if (n.form("table")) return FieldKind::table;
    if (n.form("memory")) return FieldKind::memory;
    if (n.form("global")) return FieldKind::global;
    return {};
}

ExternalKind external_kind(FieldKind kind) {
    switch (kind) {
    case FieldKind::function: return ExternalKind::function;
    case FieldKind::table: return ExternalKind::table;
    case FieldKind::memory: return ExternalKind::memory;
    default: return ExternalKind::global;
    }
}

class Assembler {
public:
    Module module(const Node& root);

private:
    using ImportName = std::pair<std::string, std::string>;

    struct Field {
        FieldKind kind;
        const Node& source;
        Cursor contents;
        std::optional<ImportName> import;
        std::vector<Export> exports;
        std::uint32_t index{};
    };

    Module module_;
    IndexSpace types_{"type", {}, {}}, functions_{"function", {}, {}}, tables_{"table", {}, {}},
               memories_{"memory", {}, {}}, globals_{"global", {}, {}},
               elements_{"element segment", {}, {}}, data_{"data segment", {}, {}};
    std::vector<Field> fields_;
    std::map<std::string, std::size_t, std::less<>> export_names_;
    bool saw_definition_ = false;
    bool needs_data_count_ = false;

    class Expression;

    IndexSpace& space(FieldKind kind);
    std::optional<ImportName> import_names(Cursor& c);
    void field(const Node& node);
    void lower(Field& field);
    void emit_export(Export export_, std::size_t pos);
    std::uint32_t resolve_signature(Signature& sig, std::size_t offset);
    BlockType block_type(Cursor& c, std::size_t offset);
    Program expression(Cursor& c);
    Program expression(const Node& n, std::string_view wrapper);
    void function(Field& field);
    void table(Field& field);
    void memory(Field& field);
    void global(Field& field);
    void element(Cursor& c);
    void element_items(Cursor& c, Element& segment, bool indices);
    void data(Cursor& c);
};

IndexSpace& Assembler::space(FieldKind kind) {
    switch (kind) {
    case FieldKind::type: return types_;
    case FieldKind::function: return functions_;
    case FieldKind::table: return tables_;
    case FieldKind::memory: return memories_;
    case FieldKind::global: return globals_;
    case FieldKind::elem: return elements_;
    case FieldKind::data: return data_;
    default: throw std::logic_error("module field has no index space");
    }
}

Module Assembler::module(const Node& root) {
    if (!root.form("module")) throw Error(root.offset, "expected a Core WAT (module ...)");
    Cursor c(root);
    optional_id(c);
    while (!c.empty()) field(c.take("module field"));
    for (auto& f : fields_) lower(f);
    if (needs_data_count_) module_.data_count = count(module_.data.size(), root.offset);
    return std::move(module_);
}

std::optional<Assembler::ImportName> Assembler::import_names(Cursor& c) {
    auto module = name(c.take("import module name"));
    auto item = name(c.take("import name"));
    return ImportName{std::move(module), std::move(item)};
}

void Assembler::field(const Node& node) {
    Cursor c(node);
    if (node.form("type")) {
        auto id = optional_id(c);
        types_.add(id, node.offset);
        const auto& def = c.take("function type");
        if (!def.form("func")) throw Error(def.offset, "only function types are supported in Core 2.0");
        Cursor type(def);
        auto sig = signature(type, true, false);
        type.done("function type");
        c.done("type definition");
        module_.types.push_back(std::move(sig.type));
        return;
    }
    if (node.form("export") || node.form("start")) {
        const auto kind = node.form("export") ? FieldKind::export_ : FieldKind::start;
        fields_.push_back({kind, node, c, {}, {}});
        return;
    }
    if (node.form("elem") || node.form("data")) {
        const auto kind = node.form("elem") ? FieldKind::elem : FieldKind::data;
        const auto* id = optional_id(c);
        const auto index = space(kind).add(id, node.offset);
        fields_.push_back({kind, node, c, {}, {}, index});
        return;
    }

    const Node* def = &node;
    std::optional<ImportName> import;
    if (node.form("import")) {
        import = import_names(c);
        def = &c.take("import descriptor");
        c.done("import");
        c = Cursor(*def);
    }
    const auto kind = external(*def);
    if (!kind) throw Error(def->offset, "unknown or unsupported Core 2.0 module field");
    const auto* id = optional_id(c);
    const auto index = space(*kind).add(id, def->offset);

    std::vector<Export> exports;
    if (!import) {
        while (c.form("export")) {
            Cursor e(c.take("inline export"));
            exports.push_back({name(e.take("export name")), external_kind(*kind), index});
            e.done("inline export");
        }
        if (c.form("import")) {
            Cursor i(c.take("inline import"));
            import = import_names(i);
            i.done("inline import");
        }
    }
    if (import) {
        if (saw_definition_) throw Error(node.offset, "import after function, table, memory, or global definition");
    } else {
        saw_definition_ = true;
        // Inline table/memory definitions expand into segments in source order.
        if (*kind == FieldKind::memory && c.form("data"))
            data_.add(nullptr, c.offset());
        if (*kind == FieldKind::table && (c.is("funcref") || c.is("externref"))) {
            auto copy = c;
            copy.take("reference type");
            if (copy.form("elem")) elements_.add(nullptr, copy.offset());
        }
    }
    fields_.push_back({*kind, node, c, std::move(import), std::move(exports), index});
}

void Assembler::emit_export(Export export_, std::size_t pos) {
    if (!export_names_.emplace(export_.name, pos).second)
        throw Error(pos, "duplicate export name");
    module_.exports.push_back(std::move(export_));
}

void Assembler::lower(Field& f) {
    for (auto& ex : f.exports) emit_export(std::move(ex), f.source.offset);
    auto& c = f.contents;
    switch (f.kind) {
    case FieldKind::type: throw std::logic_error("explicit types are lowered during declaration");
    case FieldKind::function: function(f); break;
    case FieldKind::table: table(f); break;
    case FieldKind::memory: memory(f); break;
    case FieldKind::global: global(f); break;
    case FieldKind::elem: element(c); break;
    case FieldKind::data: data(c); break;
    case FieldKind::start:
        if (module_.start) throw Error(f.source.offset, "duplicate start declaration");
        module_.start = functions_.resolve(c);
        break;
    case FieldKind::export_: {
        const auto& export_name = c.take("export name");
        const auto& def = c.take("export descriptor");
        const auto kind = external(def);
        if (!kind) throw Error(def.offset, "unknown or unsupported export kind");
        Cursor desc(def);
        emit_export({name(export_name), external_kind(*kind), space(*kind).resolve(desc)},
                    export_name.offset);
        desc.done("export descriptor");
        break;
    }
    }
    c.done("module field");
}

std::uint32_t Assembler::resolve_signature(Signature& sig, std::size_t offset) {
    if (sig.type_use) {
        const auto i = types_.resolve(*sig.type_use);
        const auto& declared = module_.types[i];
        if (sig.type.params.empty() && sig.type.results.empty()) {
            sig.type = declared;
            sig.parameter_names.resize(declared.params.size(), nullptr);
        } else if (sig.type.params != declared.params || sig.type.results != declared.results) {
            throw Error(sig.type_use->offset, "inline function signature does not match declared type");
        }
        return i;
    }
    for (std::size_t i = 0; i < module_.types.size(); ++i) {
        const auto& ty = module_.types[i];
        if (ty.params == sig.type.params && ty.results == sig.type.results)
            return static_cast<std::uint32_t>(i);
    }
    const auto index = types_.add(nullptr, offset);
    module_.types.push_back(sig.type);
    return index;
}

BlockType Assembler::block_type(Cursor& c, std::size_t offset) {
    auto sig = signature(c, false);
    if (!sig.type_use && sig.type.params.empty()) {
        if (sig.type.results.empty()) return {};
        if (sig.type.results.size() == 1) return {sig.type.results[0]};
    }
    return {resolve_signature(sig, offset)};
}

std::uint32_t natural_alignment(Opcode op) {
    if (op.byte != 0xfd) {
        // Scalar loads and stores are contiguous from 0x28 through 0x3e.
        constexpr std::array<std::uint32_t, 23> alignment{
            2, 3, 2, 3, 0, 0, 1, 1, 0, 0, 1, 1, 2, 2,
            2, 3, 2, 3, 0, 1, 0, 1, 2};
        return alignment.at(op.byte - 0x28u);
    }
    constexpr std::array<std::uint32_t, 12> alignment{4, 3, 3, 3, 3, 3, 3, 0, 1, 2, 3, 4};
    if (op.extension < alignment.size()) return alignment[op.extension];
    if (op.extension >= 0x54 && op.extension < 0x5c)
        return (op.extension - 0x54) % 4;
    return op.extension == 0x5c ? 2u : 3u; // load32_zero, load64_zero.
}

std::uint8_t lane(const Node& n, unsigned lanes) {
    const auto index = u32(n, "lane index");
    if (index >= lanes) throw Error(n.offset, "lane index out of range");
    return static_cast<std::uint8_t>(index);
}

unsigned simd_lanes(std::string_view mnemonic) {
    if (mnemonic.starts_with("i8x16.")) return 16;
    if (mnemonic.starts_with("i16x8.")) return 8;
    if (mnemonic.starts_with("i32x4.") || mnemonic.starts_with("f32x4.")) return 4;
    return 2;
}

VectorBytes vector_immediate(Cursor& c, Opcode op) {
    VectorBytes bytes{};
    if (op.extension == 0x0d) {
        for (auto& entry : bytes) entry = lane(c.take("shuffle lane"), 32);
        return bytes;
    }
    const auto& shape = c.take("SIMD lane shape");
    unsigned width;
    if (shape.is("i8x16")) width = 8;
    else if (shape.is("i16x8")) width = 16;
    else if (shape.is("i32x4") || shape.is("f32x4")) width = 32;
    else if (shape.is("i64x2") || shape.is("f64x2")) width = 64;
    else throw Error(shape.offset, "unknown SIMD constant shape");
    unsigned byte = 0;
    while (byte < bytes.size()) {
        const auto& n = c.take("SIMD lane constant");
        const auto bits = shape.is("f32x4") ? floating<float, std::uint32_t, 23>(n) :
                          shape.is("f64x2") ? floating<double, std::uint64_t, 52>(n) :
                          integer(n, width);
        for (unsigned shift = 0; shift < width; shift += 8)
            bytes[byte++] = static_cast<std::uint8_t>(bits >> shift);
    }
    return bytes;
}

class Assembler::Expression {
public:
    Expression(Assembler& owner, const IndexSpace* locals = nullptr)
        : owner_(owner), locals_(locals) {
        if (locals) labels_.push_back(nullptr); // The implicit function label.
    }

    Program read(Cursor& c) {
        sequence(c);
        result_.push_back({{0x0b}, {}, c.offset()});
        return std::move(result_);
    }

    Program read(const Node& folded_expression) {
        folded(folded_expression);
        result_.push_back({{0x0b}, {}, folded_expression.end});
        return std::move(result_);
    }

private:
    Assembler& owner_;
    const IndexSpace* locals_;
    std::vector<const Node*> labels_;
    Program result_;

    std::uint32_t label(const Node& n) const {
        if (identifier(&n)) {
            for (std::size_t depth = 0; depth < labels_.size(); ++depth) {
                const auto* id = labels_[labels_.size() - 1 - depth];
                if (id && id->text == n.text) return static_cast<std::uint32_t>(depth);
            }
            throw Error(n.offset, "unknown label identifier");
        }
        const auto depth = u32(n, "label index");
        if (depth >= labels_.size()) throw Error(n.offset, "label index out of range");
        return depth;
    }

    void enter(const Node* id, std::size_t offset) {
        const auto max = nesting_limit + (locals_ ? 1 : 0);
        if (labels_.size() == max) throw Error(offset, "control nesting limit exceeded");
        labels_.push_back(id);
    }

    void delimiter_label(Cursor& c, const Node* id) {
        if (const auto* end_id = optional_id(c); end_id && (!id || end_id->text != id->text))
            throw Error(end_id->offset, "mismatching control label");
    }

    MemoryArg memarg(Cursor& c, Opcode op) {
        MemoryArg result{natural_alignment(op), 0};
        auto attribute = [&](std::string_view key) -> std::optional<std::uint32_t> {
            const auto* n = c.peek();
            if (!n || n->kind != Node::atom || !n->text.starts_with(key)) return {};
            c.take("memory argument");
            return static_cast<std::uint32_t>(natural(n->text.substr(key.size()), UINT32_MAX,
                                                      n->offset, "memory argument"));
        };
        if (auto offset = attribute("offset=")) result.offset = *offset;
        const auto pos = c.offset();
        if (auto align = attribute("align=")) {
            if (!std::has_single_bit(*align)) throw Error(pos, "alignment must be a power of two");
            result.align = static_cast<std::uint32_t>(std::countr_zero(*align));
        }
        if (const auto* next = c.peek(); next && next->kind == Node::atom &&
            (next->text.starts_with("offset=") || next->text.starts_with("align=")))
            throw Error(next->offset, "repeated or out-of-order memory argument");
        return result;
    }

    Immediate immediate(const Node& head, const OpcodeInfo& info, Cursor& c) {
        const auto op = info.opcode;
        switch (info.format) {
        case Format::none: return {};
        case Format::block: return owner_.block_type(c, head.offset);
        case Format::index:
            switch (op.byte) {
            case 0x0c: case 0x0d: return label(c.take("branch label"));
            case 0x10: case 0xd2: return owner_.functions_.resolve(c);
            case 0x20: case 0x21: case 0x22:
                if (!locals_) throw Error(head.offset, "local instruction outside function body");
                return locals_->resolve(c);
            case 0x23: case 0x24: return owner_.globals_.resolve(c);
            case 0x25: case 0x26: return owner_.tables_.optional(c, head.offset);
            case 0xfc:
                if (op.extension == 9) {
                    owner_.needs_data_count_ = true;
                    return owner_.data_.resolve(c);
                }
                if (op.extension == 13) return owner_.elements_.resolve(c);
                return owner_.tables_.optional(c, head.offset);
            default: throw Error(head.offset, "unsupported index immediate");
            }
        case Format::index_pair:
            if (op.byte == 0x11) {
                const auto table = owner_.tables_.optional(c, head.offset);
                auto sig = signature(c, false);
                return IndexPair{owner_.resolve_signature(sig, head.offset), table};
            }
            if (op.extension == 12) {
                const auto& first = c.take("element segment index or table index");
                if (!index_token(c.peek()))
                    return IndexPair{owner_.elements_.resolve(first), owner_.tables_.check(0, head.offset)};
                return IndexPair{owner_.elements_.resolve(c), owner_.tables_.resolve(first)};
            }
            if (!index_token(c.peek())) {
                owner_.tables_.check(0, head.offset);
                return IndexPair{0, 0};
            }
            return IndexPair{owner_.tables_.resolve(c), owner_.tables_.resolve(c)};
        case Format::branch_table: {
            BranchTable table;
            table.otherwise = label(c.take("branch table label"));
            while (index_token(c.peek())) {
                table.labels.push_back(table.otherwise);
                table.otherwise = label(c.take("branch table label"));
            }
            return table;
        }
        case Format::types: {
            std::vector<Type> result;
            while (c.form("result")) {
                Cursor annotation(c.take("select result type"));
                types(annotation, result);
            }
            return result;
        }
        case Format::ref_type: {
            const auto& type = c.take("reference heap type");
            if (type.is("func")) return Type::funcref;
            if (type.is("extern")) return Type::externref;
            throw Error(type.offset, "expected func or extern heap type");
        }
        case Format::memory:
            owner_.memories_.check(0, head.offset);
            return memarg(c, op);
        case Format::memory_lane:
            owner_.memories_.check(0, head.offset);
            return MemoryLane{memarg(c, op), lane(c.take("memory lane index"), 16u >> natural_alignment(op))};
        case Format::lane: return lane(c.take("lane index"), simd_lanes(info.mnemonic));
        case Format::zero:
        case Format::zero_zero:
            owner_.memories_.check(0, head.offset);
            return {};
        case Format::index_zero:
            owner_.memories_.check(0, head.offset);
            owner_.needs_data_count_ = true;
            return owner_.data_.resolve(c);
        case Format::i32: return static_cast<std::uint32_t>(integer(c.take("i32 constant"), 32));
        case Format::i64: return integer(c.take("i64 constant"), 64);
        case Format::f32: return floating<float, std::uint32_t, 23>(c.take("f32 constant"));
        case Format::f64: return floating<double, std::uint64_t, 52>(c.take("f64 constant"));
        case Format::vector_bytes: return vector_immediate(c, op);
        }
        throw Error(head.offset, "unsupported instruction immediate");
    }

    const OpcodeInfo& instruction(const Node& head, const Cursor& c) {
        const auto mnemonic = atom(head, "instruction mnemonic");
        if (head.is("else") || head.is("end"))
            throw Error(head.offset, "unexpected control delimiter");
        if (const auto* info = lookup(mnemonic, mnemonic == "select" && c.form("result")))
            return *info;
        throw Error(head.offset, "unknown or unsupported Core 2.0 instruction '" +
                                 std::string(mnemonic.substr(0, 64)) + "'");
    }

    void sequence(Cursor& c, bool delimiters = false) {
        while (!c.empty() && !(delimiters && (c.is("end") || c.is("else")))) {
            const auto& head = c.take("instruction");
            if (head.kind == Node::list) {
                folded(head);
                continue;
            }
            const auto& info = instruction(head, c);
            if (info.format != Format::block) {
                result_.push_back({info.opcode, immediate(head, info, c), head.offset});
                continue;
            }
            const auto* id = optional_id(c);
            result_.push_back({info.opcode, immediate(head, info, c), head.offset});
            enter(id, head.offset);
            sequence(c, true);
            if (c.is("else") && info.opcode.byte == 0x04) {
                const auto& otherwise = c.take("else");
                delimiter_label(c, id);
                result_.push_back({{0x05}, {}, otherwise.offset});
                sequence(c, true);
            }
            if (!c.is("end")) throw Error(c.offset(), "expected end for structured instruction");
            result_.push_back({{0x0b}, {}, c.take("end").offset});
            delimiter_label(c, id);
            labels_.pop_back();
        }
    }

    void folded(const Node& form) {
        if (form.kind != Node::list) throw Error(form.offset, "expected a folded instruction");
        Cursor c(form, 0);
        const auto& head = c.take("instruction mnemonic");
        const auto& info = instruction(head, c);
        if (info.format != Format::block) {
            auto operand = immediate(head, info, c);
            while (!c.empty()) folded(c.take("folded instruction operand"));
            result_.push_back({info.opcode, std::move(operand), head.offset});
            return;
        }
        const auto* id = optional_id(c);
        auto type = immediate(head, info, c);
        if (info.opcode.byte == 0x04) {
            // Condition operands are outside the label introduced by the if.
            while (!c.empty() && !c.form("then"))
                folded(c.take("folded condition instruction or (then ...)"));
            if (!c.form("then")) throw Error(c.offset(), "expected (then ...) in folded if");
        }
        result_.push_back({info.opcode, std::move(type), head.offset});
        enter(id, head.offset);
        if (info.opcode.byte == 0x04) {
            Cursor then(c.take("then clause"));
            sequence(then);
            if (c.form("else")) {
                const auto& node = c.take("else clause");
                result_.push_back({{0x05}, {}, node.offset});
                Cursor otherwise(node);
                sequence(otherwise);
            }
            c.done("folded if");
        } else {
            sequence(c);
        }
        result_.push_back({{0x0b}, {}, form.end});
        labels_.pop_back();
    }
};

Program Assembler::expression(Cursor& c) {
    return Expression(*this).read(c);
}

Program Assembler::expression(const Node& n, std::string_view wrapper) {
    if (n.form(wrapper)) {
        Cursor c(n);
        return expression(c);
    }
    return Expression(*this).read(n);
}

void Assembler::function(Field& f) {
    auto& c = f.contents;
    auto sig = signature(c, true);
    const auto type = resolve_signature(sig, f.source.offset);
    IndexSpace locals{"local", {}, {}};
    for (const auto* id : sig.parameter_names) locals.add(id, f.source.offset);
    if (f.import) {
        c.done("function import");
        module_.imports.push_back({std::move(f.import->first), std::move(f.import->second), type});
        return;
    }

    FunctionBody body{c.offset(), {}, {}};
    const auto local = [&](Type ty, const Node* id, std::size_t offset) {
        locals.add(id, offset);
        if (!body.locals.empty() && body.locals.back().type == ty)
            ++body.locals.back().count;
        else
            body.locals.push_back({1, ty});
    };
    while (c.form("local")) {
        const auto& n = c.take("local declaration");
        Cursor decl(n);
        if (auto* id = optional_id(decl)) {
            local(value_type(decl.take("local type")), id, n.offset);
            decl.done("named local declaration");
        } else {
            while (!decl.empty()) local(value_type(decl.take("local type")), nullptr, n.offset);
        }
    }
    body.instructions = Expression(*this, &locals).read(c);
    module_.functions.push_back({type, std::move(body)});
}

void Assembler::table(Field& f) {
    auto& c = f.contents;
    if (!f.import && (c.is("funcref") || c.is("externref"))) {
        const auto type = ref_type(c.take("table element type"));
        const auto& elem = c.take("inline element segment");
        if (!elem.form("elem")) throw Error(elem.offset, "expected (elem ...) after table type");
        Element segment{SegmentMode::active, f.index,
                        {{{0x41}, std::uint32_t{0}, elem.offset}, {{0x0b}, {}, elem.end}},
                        type, {}};
        Cursor entries(elem);
        element_items(entries, segment, entries.empty() || index_token(entries.peek()));
        const auto size = count(segment.init.size(), elem.offset);
        module_.tables.push_back({type, {size, size}});
        module_.elements.push_back(std::move(segment));
        return;
    }
    auto bound = limits(c);
    TableType type{ref_type(c.take("table reference type")), bound};
    if (f.import)
        module_.imports.push_back({std::move(f.import->first), std::move(f.import->second), type});
    else
        module_.tables.push_back(type);
}

void Assembler::memory(Field& f) {
    auto& c = f.contents;
    if (f.index != 0)
        throw Error(f.source.offset, "Core 2.0 supports only one memory");
    if (!f.import && c.form("data")) {
        const auto& n = c.take("inline data segment");
        Cursor payload(n);
        auto bytes = data_bytes(payload);
        const auto pages = count(bytes.size() / 65536 + (bytes.size() % 65536 != 0), n.offset);
        module_.memories.push_back({{pages, pages}});
        module_.data.push_back({SegmentMode::active, f.index,
                                {{{0x41}, std::uint32_t{0}, n.offset}, {{0x0b}, {}, n.end}},
                                std::move(bytes)});
        return;
    }
    const MemoryType type{limits(c)};
    if (f.import)
        module_.imports.push_back({std::move(f.import->first), std::move(f.import->second), type});
    else
        module_.memories.push_back(type);
}

void Assembler::global(Field& f) {
    auto& c = f.contents;
    const auto type = global_type(c.take("global type"));
    if (f.import)
        module_.imports.push_back({std::move(f.import->first), std::move(f.import->second), type});
    else
        module_.globals.push_back({type, expression(c)});
}

void Assembler::element_items(Cursor& c, Element& segment, bool indices) {
    if (indices && segment.type != Type::funcref && !c.empty())
        throw Error(c.offset(), "function indices require a funcref element segment");
    while (!c.empty()) {
        const auto& item = c.take("element initializer");
        if (indices) {
            segment.init.push_back({{{0xd2}, functions_.resolve(item), item.offset},
                                    {{0x0b}, {}, item.offset}});
        } else {
            segment.init.push_back(expression(item, "item"));
        }
    }
}

void Assembler::element(Cursor& c) {
    Element segment{SegmentMode::passive, 0, {}, Type::funcref, {}};
    const bool explicit_table = c.form("table");
    if (c.is("declare")) {
        c.take("declare");
        segment.mode = SegmentMode::declarative;
    } else if (explicit_table || (c.peek() && c.peek()->kind == Node::list)) {
        segment.mode = SegmentMode::active;
        if (explicit_table) {
            Cursor table(c.take("table use"));
            segment.table = tables_.resolve(table);
            table.done("table use");
        } else {
            tables_.check(0, c.offset());
        }
        segment.offset = expression(c.take("element offset"), "offset");
    }
    if (c.is("func")) {
        c.take("element kind");
        element_items(c, segment, true);
    } else if (c.is("funcref") || c.is("externref")) {
        segment.type = ref_type(c.take("element reference type"));
        element_items(c, segment, false);
    } else if (segment.mode == SegmentMode::active && !explicit_table) {
        element_items(c, segment, true); // Legacy active funcidx list.
    } else {
        throw Error(c.offset(), "expected func, funcref, or externref element type");
    }
    module_.elements.push_back(std::move(segment));
}

void Assembler::data(Cursor& c) {
    Data segment{SegmentMode::passive, 0, {}, {}};
    if (c.form("memory") || (c.peek() && c.peek()->kind == Node::list)) {
        segment.mode = SegmentMode::active;
        if (c.form("memory")) {
            Cursor memory(c.take("memory use"));
            segment.memory = memories_.resolve(memory);
            memory.done("memory use");
        } else {
            memories_.check(0, c.offset());
        }
        segment.offset = expression(c.take("data offset"), "offset");
    }
    segment.bytes = data_bytes(c);
    module_.data.push_back(std::move(segment));
}

} // namespace

Module assemble_module(std::string_view input) {
    const auto root = Reader(input).document();
    return Assembler().module(root);
}

} // namespace limestone::wasmutils::core
