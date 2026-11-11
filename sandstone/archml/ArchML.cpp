#include "ArchML.hpp"
#include "Parser.h"
#include <charconv>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <memory>
#include <set>
#include <sstream>

namespace sandstone::archml {
namespace {
Location locate(const std::vector<std::size_t> &lines, std::size_t offset) {
    const auto line = std::upper_bound(lines.begin(), lines.end(), offset) - lines.begin();
    return {offset, static_cast<std::size_t>(line), offset - lines[line - 1] + 1};
}
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::size_t invalid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto start = i;
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count = 0, value = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 0x10000; }
        else return start;
        if (text.size() - i < count) return start;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return start;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return start;
    }
    return std::string_view::npos;
}
Node convert(const archml_syntax_node *expr, const std::vector<std::size_t> &lines, const std::string &source) {
    if (!expr) throw std::bad_alloc();
    Node node;
    node.location = locate(lines, archml_syntax_offset(expr));
    if (archml_syntax_kind(expr) == ARCHML_LIST) {
        node.kind = Node::Kind::List;
        const auto count = archml_syntax_count(expr);
        node.children.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            node.children.push_back(convert(archml_syntax_child(expr, i), lines, source));
    } else if (archml_syntax_kind(expr) == ARCHML_SYMBOL) {
        std::size_t size;
        const auto *text = archml_syntax_text(expr, &size);
        node.text.assign(text, size);
    } else {
        node.kind = Node::Kind::String;
        std::size_t size;
        const auto *spelling = archml_syntax_text(expr, &size);
        std::string_view text(spelling, size);
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            char c = text[i];
            if (c == '\\') {
                c = text[++i];
                switch (c) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '\\': case '"': break;
                case 'x':
                    if (i + 2 >= text.size() - 1 || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0)
                        throw Error(source, node.location, "expected two hexadecimal digits after \\x");
                    c = static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
                    i += 2;
                    break;
                default: throw Error(source, node.location, "unknown string escape");
                }
            }
            node.text += c;
        }
        if (invalid_utf8(node.text) != std::string_view::npos)
            throw Error(source, node.location, "string is not valid UTF-8");
    }
    return node;
}
std::uint64_t integer(const Node &node, const std::string &source, bool allow_negative = false) {
    auto text = std::string_view(node.text);
    bool negative = !text.empty() && text.front() == '-';
    if (negative) text.remove_prefix(1);
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) { base = 16; text.remove_prefix(2); }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (node.kind != Node::Kind::Symbol || text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || (negative && (!allow_negative || value > (UINT64_C(1) << 63))))
        throw Error(source, node.location, "invalid or out-of-range integer '" + node.text + "'");
    return negative ? std::uint64_t(0) - value : value;
}
} // namespace

Error::Error(std::string source, Location at, std::string message)
    : std::runtime_error(source + ":" + std::to_string(at.line) + ":" +
                         std::to_string(at.column) + ": " + message), location(at) {}

std::string_view Node::head() const {
    return kind == Kind::List && !children.empty() && children[0].kind == Kind::Symbol
        ? std::string_view(children[0].text) : std::string_view{};
}
const Node *Node::property(std::string_view name) const {
    for (const auto &child : children)
        if (child.kind == Kind::List && child.head() == name) return &child;
    return nullptr;
}
[[noreturn]] void Document::fail(const Node &node, const std::string &message) const {
    throw Error(source_, node.location, message);
}
Document Document::parse(std::string_view text, std::string source) {
    std::vector<std::size_t> lines{0};
    for (std::size_t i = 0; i < text.size(); ++i) if (text[i] == '\n') lines.push_back(i + 1);
    if (const auto nul = text.find('\0'); nul != std::string_view::npos)
        throw Error(source, locate(lines, nul), "NUL byte in source");
    if (const auto invalid = invalid_utf8(text); invalid != std::string_view::npos)
        throw Error(source, locate(lines, invalid), "source is not valid UTF-8");
    std::string input(text);
    std::unique_ptr<archml_syntax, decltype(&archml_syntax_free)> ast(nullptr, archml_syntax_free);
    std::size_t error = 0;
    {
        static std::mutex parser_mutex;
        std::lock_guard lock(parser_mutex);
        ast.reset(archml_parse_syntax(input.c_str(), &error));
    }
    if (!ast) throw Error(source, locate(lines, error), "invalid ArchML syntax (or nesting limit exceeded)");
    Document doc;
    doc.source_ = std::move(source);
    doc.root_ = convert(archml_syntax_root(ast.get()), lines, doc.source_);
    doc.validate();
    return doc;
}
Document Document::load(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path);
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) throw std::runtime_error("cannot read " + path);
    return parse(text, path);
}
const Node *Document::find(std::string_view kind, std::string_view name) const {
    auto entry = records_.find({std::string(kind), std::string(name)});
    return entry == records_.end() ? nullptr : &root_.children[entry->second];
}
const runtime::Instruction *Document::instruction(std::string_view name) const {
    return runtime::find(instructions_, name);
}
std::vector<std::uint8_t> Document::encode(std::string_view name, std::span<const std::uint64_t> values) const {
    const auto *form = instruction(name);
    if (!form) throw std::invalid_argument("unknown instruction '" + std::string(name) + "'");
    return runtime::encode(*form, values);
}
bool Document::decode(std::span<const std::uint8_t> bytes, runtime::Decoded &result) const {
    return runtime::decode(instructions_, bytes, result);
}

void Document::validate() {
    auto symbol = [&](const Node &node) -> const std::string & {
        if (node.kind != Node::Kind::Symbol || node.text.empty()) fail(node, "expected symbol");
        return node.text;
    };
    auto arity = [&](const Node &node, std::size_t count) {
        if (node.kind != Node::Kind::List || node.children.size() != count + 1)
            fail(node, "expected " + std::to_string(count) + " arguments to '" + std::string(node.head()) + "'");
    };
    auto property = [&](const Node &node, std::string_view name) -> const Node & {
        auto *value = node.property(name);
        if (!value) fail(node, "missing '" + std::string(name) + "'");
        return *value;
    };
    auto scalar = [&](const Node &node, std::string_view name) -> const Node & {
        const auto &value = property(node, name);
        arity(value, 1);
        return value.children[1];
    };
    auto number = [&](const Node &node) { return integer(node, source_); };
    auto properties = [&](const Node &node, std::size_t start) {
        std::set<std::string> names;
        for (std::size_t i = start; i < node.children.size(); ++i) {
            const auto &child = node.children[i];
            if (child.head().empty()) fail(child, "expected named property");
            if (!names.insert(std::string(child.head())).second)
                fail(child, "duplicate property '" + std::string(child.head()) + "'");
        }
    };
    if (root_.head() != "archml" || root_.children.size() < 4)
        fail(root_, "expected (archml 1 name (target name ...) ...)");
    if (number(root_.children[1]) != 1) fail(root_.children[1], "unsupported ArchML version");
    name_ = symbol(root_.children[2]);
    std::size_t targets = 0;
    for (std::size_t i = 3; i < root_.children.size(); ++i) {
        const auto &record = root_.children[i];
        if (record.head().empty() || record.children.size() < 2) fail(record, "expected named record");
        const auto &name = symbol(record.children[1]);
        if (!records_.emplace(std::make_pair(std::string(record.head()), name), i).second)
            fail(record, "duplicate record '" + std::string(record.head()) + " " + name + "'");
        properties(record, 2);
        if (record.head() == "target") {
            ++targets;
            if (name != name_) fail(record, "target name must match document name");
            const auto bits = number(scalar(record, "bits"));
            if (bits == 0 || bits > 65536) fail(record, "invalid target bit width");
            const auto &endian = symbol(scalar(record, "endian"));
            if (endian != "little" && endian != "big") fail(record, "expected little or big endian");
        }
    }
    if (targets != 1) fail(root_, "expected exactly one target record");
    std::function<void(const Node &)> references = [&](const Node &node) {
        if (node.head() == "ref") {
            arity(node, 2);
            if (!find(symbol(node.children[1]), symbol(node.children[2])))
                fail(node, "unresolved reference '" + node.children[1].text + " " + node.children[2].text + "'");
        }
        for (const auto &child : node.children) references(child);
    };
    references(root_);

    // Feature dependencies form a DAG; retain source order for emission.
    std::map<std::string, int> feature_state;
    std::function<void(const Node &)> feature = [&](const Node &record) {
        auto &state = feature_state[record.children[1].text];
        if (state == 1) fail(record, "cyclic feature dependency");
        if (state == 2) return;
        state = 1;
        if (auto *dependencies = record.property("requires"))
            for (std::size_t i = 1; i < dependencies->children.size(); ++i) {
                const auto &ref = dependencies->children[i];
                if (ref.head() != "ref" || ref.children[1].text != "feature")
                    fail(ref, "expected feature reference");
                feature(*find("feature", ref.children[2].text));
            }
        state = 2;
    };
    for (std::size_t i = 3; i < root_.children.size(); ++i) {
        const auto &record = root_.children[i];
        if (record.head() == "feature") feature(record);
        if (record.head() == "pcode-op") {
            const auto &range = property(record, "arity");
            arity(range, 2);
            if (number(range.children[1]) > number(range.children[2])) fail(range, "invalid arity range");
        }
        if (record.head() == "storage" || record.head() == "register" || record.head() == "regclass" || record.head() == "type") {
            if (number(scalar(record, "bits")) == 0) fail(record, "bit width must be positive");
        }
        if (record.head() == "register") {
            const auto &storage = scalar(record, "storage");
            const auto *backing = find("storage", symbol(storage));
            if (!backing) fail(storage, "unknown register storage");
            const auto bits = number(scalar(record, "bits"));
            const auto offset = number(scalar(record, "offset"));
            const auto capacity = number(scalar(*backing, "bits"));
            if (offset >= capacity || bits > capacity - offset) fail(record, "register view exceeds storage");
            if (record.property("encoding")) (void)number(scalar(record, "encoding"));
        }
        if (record.head() == "regclass") {
            const auto &members = property(record, "members");
            std::set<std::string> names;
            for (std::size_t j = 1; j < members.children.size(); ++j) {
                const auto &member = members.children[j];
                if (member.head() != "ref" || member.children[1].text != "register") fail(member, "expected register reference");
                if (!names.insert(member.children[2].text).second) fail(member, "duplicate register class member");
            }
        }
    }
    std::set<std::string> form_names;
    for (std::size_t r = 3; r < root_.children.size(); ++r) {
        const auto &record = root_.children[r];
        const bool binary = record.head() == "instruction";
        if (!binary && record.head() != "operation") continue;
        runtime::Instruction form;
        form.name = record.children[1].text;
        if (!form_names.insert(form.name).second) fail(record, "instruction/operation name collision");
        const auto &assembly = scalar(record, "assembly");
        if (assembly.kind != Node::Kind::String) fail(assembly, "assembly must be a string");
        form.assembly = assembly.text;
        auto refs = [&](std::string_view key, std::string_view kind, std::vector<std::string> &out) {
            if (const auto *list = record.property(key))
                for (std::size_t i = 1; i < list->children.size(); ++i) {
                    const auto &ref = list->children[i];
                    if (ref.head() != "ref" || ref.children[1].text != kind)
                        fail(ref, "expected " + std::string(kind) + " reference");
                    out.push_back(ref.children[2].text);
                }
        };
        refs("requires", "feature", form.features);
        refs("modes", "mode", form.modes);
        const auto &operands = property(record, "operands");
        std::map<std::string, std::size_t> operand_index;
        for (std::size_t i = 1; i < operands.children.size(); ++i) {
            const auto &node = operands.children[i];
            if (node.head().empty()) fail(node, "expected operand definition");
            properties(node, 1);
            runtime::Operand operand;
            operand.name = std::string(node.head());
            if (!operand_index.emplace(operand.name, form.operands.size()).second) fail(node, "duplicate operand");
            operand.kind = symbol(scalar(node, "kind"));
            if (operand.kind != "register" && operand.kind != "immediate" && operand.kind != "value")
                fail(node, "unsupported operand kind");
            if (binary && operand.kind == "value") fail(node, "binary instruction cannot encode a virtual value");
            const auto bits = number(scalar(node, "bits"));
            if (bits == 0 || bits > 64) fail(node, "operand width must be between 1 and 64");
            operand.bits = static_cast<unsigned>(bits);
            const auto &sign = symbol(scalar(node, "signed"));
            if (sign != "true" && sign != "false") fail(node, "signed must be true or false");
            operand.is_signed = sign == "true";
            operand.scale = number(scalar(node, "scale"));
            if (operand.scale == 0 || operand.scale > INT64_MAX) fail(node, "invalid operand scale");
            const auto &access = symbol(scalar(node, "access"));
            if (access != "read" && access != "write" && access != "read-write") fail(node, "invalid operand access");
            if (const auto *excluded = node.property("exclude"))
                for (std::size_t j = 1; j < excluded->children.size(); ++j)
                    operand.excluded.push_back(integer(excluded->children[j], source_, operand.is_signed));
            if (operand.kind == "register") {
                if (operand.is_signed || operand.scale != 1) fail(node, "register operands must be unsigned with scale 1");
                const auto &ref = scalar(node, "class");
                if (ref.head() != "ref" || ref.children[1].text != "regclass") fail(ref, "expected regclass reference");
                const auto &members = property(*find("regclass", ref.children[2].text), "members");
                for (std::size_t j = 1; j < members.children.size(); ++j) {
                    const auto &member = members.children[j];
                    if (member.head() != "ref" || member.children[1].text != "register") fail(member, "expected register reference");
                    const auto *reg = find("register", member.children[2].text);
                    const auto value = number(scalar(*reg, "encoding"));
                    if (value <= runtime::mask(operand.bits)) operand.registers.push_back(value);
                }
                std::sort(operand.registers.begin(), operand.registers.end());
                operand.registers.erase(std::unique(operand.registers.begin(), operand.registers.end()), operand.registers.end());
                if (operand.registers.empty()) fail(node, "register class has no encodable members");
            }
            form.operands.push_back(std::move(operand));
        }
        // Templates may contain presentation-only bindings, e.g. a branch's
        // {target} label rather than its encoded byte-valued {disp} operand.
        std::map<std::string, std::string> bindings;
        for (std::size_t i = 0; i < form.assembly.size(); ++i) {
            if (form.assembly[i] == '}') fail(assembly, "unmatched assembly brace");
            if (form.assembly[i] != '{') continue;
            const auto end = form.assembly.find('}', i + 1);
            if (end == std::string::npos || end == i + 1 || form.assembly.find('{', i + 1) < end)
                fail(assembly, "invalid assembly placeholder");
            bindings.emplace(form.assembly.substr(i + 1, end - i - 1), "");
            i = end;
        }
        try { runtime::render(form, std::vector<std::string>(form.operands.size()), bindings); }
        catch (const std::invalid_argument &error) { fail(assembly, error.what()); }

        const auto &pcode = scalar(record, "pcode");
        std::function<void(const Node &)> check_pcode = [&](const Node &expr) {
            if (expr.kind != Node::Kind::List) return;
            const auto *op = find("pcode-op", expr.head());
            if (!op) fail(expr, "unknown P-code operator '" + std::string(expr.head()) + "'");
            const auto &range = property(*op, "arity");
            const auto count = expr.children.size() - 1;
            if (count < number(range.children[1]) || count > number(range.children[2])) fail(expr, "incorrect P-code arity");
            if (expr.head() == "read" || expr.head() == "write") {
                arity(expr, expr.head() == "read" ? 1 : 2);
                if (!operand_index.contains(symbol(expr.children[1]))) fail(expr, "unknown P-code operand");
            }
            if (expr.head() == "reg" || expr.head() == "set-reg") {
                arity(expr, expr.head() == "reg" ? 1 : 2);
                if (!find("register", symbol(expr.children[1]))) fail(expr, "unknown P-code register");
            }
            for (std::size_t i = 1; i < expr.children.size(); ++i) check_pcode(expr.children[i]);
        };
        if (pcode.kind != Node::Kind::List) fail(pcode, "expected P-code expression");
        check_pcode(pcode);

        const auto *encoding = record.property("encoding");
        if (!binary) {
            if (encoding) fail(*encoding, "operation cannot have a binary encoding");
        } else {
            if (!encoding) fail(record, "instruction requires an encoding");
            const auto &base = scalar(*encoding, "base");
            if (base.kind != Node::Kind::String || base.text.empty() || base.text.size() % 2)
                fail(base, "encoding base must be a nonempty even-length hex string");
            for (std::size_t i = 0; i < base.text.size(); i += 2) {
                const auto high = hex(base.text[i]), low = hex(base.text[i + 1]);
                if (high < 0 || low < 0) fail(base, "invalid hexadecimal encoding byte");
                form.base.push_back(static_cast<std::uint8_t>(high * 16 + low));
            }
            std::vector<bool> occupied(form.base.size() * 8);
            std::vector<std::uint64_t> covered(form.operands.size());
            std::size_t bases = 0;
            for (std::size_t i = 1; i < encoding->children.size(); ++i) {
                const auto &field = encoding->children[i];
                if (field.head() == "base") { ++bases; continue; }
                if (field.head() != "field") fail(field, "unsupported encoding property");
                arity(field, 4);
                const auto op = operand_index.find(symbol(field.children[1]));
                if (op == operand_index.end()) fail(field, "unknown encoding operand");
                const auto offset = number(field.children[2]);
                const auto width = number(field.children[3]);
                const auto shift = number(field.children[4]);
                if (width == 0 || width > 64 || shift >= 64 || width > form.operands[op->second].bits ||
                    shift > form.operands[op->second].bits - width || offset >= occupied.size() || width > occupied.size() - offset)
                    fail(field, "encoding field is out of bounds");
                const auto bits = runtime::mask(static_cast<unsigned>(width)) << shift;
                if (covered[op->second] & bits) fail(field, "operand bit is encoded more than once");
                covered[op->second] |= bits;
                for (std::size_t bit = offset; bit < offset + width; ++bit) {
                    if (occupied[bit]) fail(field, "overlapping encoding fields");
                    if (form.base[bit / 8] & (1u << (bit % 8))) fail(field, "encoding field overlaps fixed base bits");
                    occupied[bit] = true;
                }
                form.fields.push_back({op->second, static_cast<std::size_t>(offset), static_cast<unsigned>(width), static_cast<unsigned>(shift)});
            }
            if (bases != 1) fail(*encoding, "expected exactly one encoding base");
            for (std::size_t i = 0; i < covered.size(); ++i)
                if (covered[i] != runtime::mask(form.operands[i].bits)) fail(*encoding, "encoding does not cover all operand bits");
        }
        instructions_.push_back(std::move(form));
    }
}

std::string quote(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : text) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        default:
            if (c < 32) { result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15]; }
            else result += static_cast<char>(c);
        }
    }
    return result + '"';
}
std::string format(const Node &node) {
    if (node.kind == Node::Kind::Symbol) return node.text;
    if (node.kind == Node::Kind::String) {
        // ArchML uses byte escapes; JSON's \u00 escapes are rewritten below.
        auto text = quote(node.text);
        // Only rewrite actual escape tokens, never a literal escaped backslash.
        std::string result;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                if (text[i + 1] == 'u') { result += "\\x"; result += text.substr(i + 4, 2); i += 5; }
                else { result += text[i]; result += text[++i]; }
            } else result += text[i];
        }
        return result;
    }
    std::string result = "(";
    for (const auto &child : node.children) { if (result.size() > 1) result += ' '; result += format(child); }
    return result + ')';
}
std::string json(const Node &node) {
    if (node.kind == Node::Kind::Symbol) return "{\"symbol\":" + quote(node.text) + '}';
    if (node.kind == Node::Kind::String) return quote(node.text);
    std::string result = "[";
    for (const auto &child : node.children) { if (result.size() > 1) result += ','; result += json(child); }
    return result + ']';
}
} // namespace sandstone::archml
