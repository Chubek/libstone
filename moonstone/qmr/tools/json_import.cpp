#include "json_import.hpp"

extern "C" {
#include "kson.h"
}

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moonstone::qmr::cli {
namespace {

[[noreturn]] void fail_at(std::size_t line, std::size_t column, const std::string &why) {
    throw std::runtime_error("JSON " + std::to_string(line) + ":" + std::to_string(column) +
                             ": " + why);
}

std::pair<std::size_t, std::size_t> locate(std::string_view text, std::size_t pos) {
    std::size_t line = 1, column = 1;
    for (std::size_t i = 0; i < pos && i < text.size(); ++i) {
        if (text[i] == '\n') { ++line; column = 1; } else ++column;
    }
    return {line, column};
}

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

void append_utf8(std::string &out, unsigned long codepoint) {
    if (codepoint <= 0x7f) {
        out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7ff) {
        out += static_cast<char>(0xc0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        out += static_cast<char>(0xe0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes a validated JSON string body (between quotes) into UTF-8 text.
std::string decode_string(std::string_view body, std::string_view whole, std::size_t at) {
    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\') { out += body[i]; continue; }
        ++i;
        const char escape = body[i];
        switch (escape) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            unsigned long code = 0;
            for (int k = 1; k <= 4; ++k) code = code * 16 + hex_value(body[i + k]);
            if (code >= 0xd800 && code <= 0xdbff) {
                // Validated surrogate pair: decode the low surrogate.
                i += 6; // skip "XXXX\uD..." head, lands on 'u' of the second escape
                unsigned long low = 0;
                for (int k = 2; k <= 5; ++k) low = low * 16 + hex_value(body[i + k]);
                i += 4;
                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            }
            append_utf8(out, code);
            i += 4;
            break;
        }
        default:
            (void)whole; (void)at;
            throw std::runtime_error("JSON: internal error decoding validated escape");
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Strict RFC 8259 validator. Iterative (bounded stack) so hostile nesting can
// never overflow the C++ stack. Guarantees anything it accepts is exactly
// the grammar kson will see, with no non-standard extensions.
// ---------------------------------------------------------------------------
class Validator {
public:
    Validator(std::string_view text, std::size_t max_depth)
        : text_(text), max_depth_(max_depth) {}

private:
    enum class Container { Array, Object };
    enum class Phase { Value, Key, CommaOrEnd };
    struct Frame { Container container; Phase phase; };

    std::string_view text_;
    std::size_t max_depth_;
    std::size_t pos_ = 0;
    std::vector<Frame> stack_;

    [[noreturn]] void fail_here(const char *why) {
        const auto [line, column] = locate(text_, pos_);
        fail_at(line, column, why);
    }
    void skip_ws() {
        while (pos_ < text_.size() && is_ws(text_[pos_])) ++pos_;
    }
    char peek() {
        if (pos_ >= text_.size()) fail_here("unexpected end of JSON input");
        return text_[pos_];
    }
    void expect(char c, const char *what) {
        if (peek() != c) fail_here(what);
        ++pos_;
    }
    void expect_word(const char *word) {
        for (const char *w = word; *w; ++w) {
            if (pos_ >= text_.size() || text_[pos_++] != *w)
                fail_here("invalid JSON literal");
        }
    }

    void string_token() {
        expect('"', "expected JSON string");
        std::size_t i = pos_;
        for (;;) {
            if (i >= text_.size()) { pos_ = i; fail_here("unterminated JSON string"); }
            const char c = text_[i];
            if (c == '"') break;
            if (static_cast<unsigned char>(c) < 0x20) { pos_ = i; fail_here("control character in JSON string"); }
            if (c == '\\') {
                ++i;
                if (i >= text_.size()) { pos_ = i; fail_here("unterminated JSON escape"); }
                const char e = text_[i];
                switch (e) {
                case '"': case '\\': case '/': case 'b': case 'f': case 'n':
                case 'r': case 't':
                    ++i;
                    break;
                case 'u': {
                    if (i + 4 >= text_.size()) { pos_ = i; fail_here("truncated \\u escape"); }
                    unsigned long code = 0;
                    for (int k = 1; k <= 4; ++k) {
                        const int digit = hex_value(text_[i + k]);
                        if (digit < 0) { pos_ = i + k; fail_here("invalid \\u escape"); }
                        code = code * 16 + static_cast<unsigned long>(digit);
                    }
                    i += 5;
                    if (code >= 0xdc00 && code <= 0xdfff) { pos_ = i - 6; fail_here("unpaired low surrogate"); }
                    if (code >= 0xd800 && code <= 0xdbff) {
                        // must be followed by \uDC00-\uDFFF
                        if (i + 1 >= text_.size() || text_[i] != '\\' || text_[i + 1] != 'u') {
                            pos_ = i; fail_here("unpaired high surrogate");
                        }
                        unsigned long low = 0;
                        for (int k = 2; k <= 5; ++k) {
                            const int digit = hex_value(text_[i + k]);
                            if (digit < 0) { pos_ = i + k; fail_here("invalid \\u escape"); }
                            low = low * 16 + static_cast<unsigned long>(digit);
                        }
                        if (low < 0xdc00 || low > 0xdfff) { pos_ = i; fail_here("unpaired high surrogate"); }
                        i += 6;
                    }
                    break;
                }
                default:
                    pos_ = i; fail_here("invalid JSON escape");
                }
            } else {
                ++i;
            }
        }
        pos_ = i + 1;
    }

    void number_token() {
        std::size_t i = pos_;
        if (i < text_.size() && text_[i] == '-') ++i;
        if (i >= text_.size() || !is_digit(text_[i])) { pos_ = i; fail_here("invalid JSON number"); }
        if (text_[i] == '0') {
            ++i;
        } else {
            while (i < text_.size() && is_digit(text_[i])) ++i;
        }
        if (i < text_.size() && text_[i] == '.') {
            ++i;
            if (i >= text_.size() || !is_digit(text_[i])) { pos_ = i; fail_here("digit required after decimal point"); }
            while (i < text_.size() && is_digit(text_[i])) ++i;
        }
        if (i < text_.size() && (text_[i] == 'e' || text_[i] == 'E')) {
            ++i;
            if (i < text_.size() && (text_[i] == '+' || text_[i] == '-')) ++i;
            if (i >= text_.size() || !is_digit(text_[i])) { pos_ = i; fail_here("digit required in exponent"); }
            while (i < text_.size() && is_digit(text_[i])) ++i;
        }
        pos_ = i;
    }

    void expect_value() {
        skip_ws();
        const char c = peek();
        switch (c) {
        case '{':
            push(Container::Object, Phase::Key);
            ++pos_;
            skip_ws();
            if (peek() == '}') { ++pos_; pop(); }
            break;
        case '[':
            push(Container::Array, Phase::Value);
            ++pos_;
            skip_ws();
            if (peek() == ']') { ++pos_; pop(); }
            else expect_value(); // first element
            break;
        case '"':
            string_token();
            break;
        case 't': expect_word("true"); break;
        case 'f': expect_word("false"); break;
        case 'n': expect_word("null"); break;
        default:
            if (c == '-' || is_digit(c)) number_token();
            else fail_here("unexpected character");
        }
    }

    void push(Container container, Phase phase) {
        if (stack_.size() >= max_depth_) fail_here("JSON nesting exceeds contract depth");
        stack_.push_back({container, phase});
    }
    void pop() { stack_.pop_back(); }

    void loop_members() {
        // After a member/element, drive the container state machine. The
        // back frame is re-acquired on every access: expect_value() may push
        // nested frames (reallocating the vector).
        while (!stack_.empty()) {
            skip_ws();
            const Container container = stack_.back().container;
            const Phase phase = stack_.back().phase;
            const char c = peek();
            if (container == Container::Array) {
                if (c == ']') { ++pos_; stack_.pop_back(); continue; }
                if (c != ',') fail_here("expected ',' or ']' in JSON array");
                ++pos_;
                expect_value();
                continue;
            }
            switch (phase) {
            case Phase::Key:
                if (c == '}') { ++pos_; stack_.pop_back(); break; }
                if (c != '"') fail_here("expected JSON string key or '}'");
                string_token();
                skip_ws();
                expect(':', "expected ':' after JSON key");
                // Transition before the value: nested containers opened by
                // expect_value() go on top and are driven to completion by
                // this same loop; scalars return here already in member-done
                // state.
                stack_.back().phase = Phase::CommaOrEnd;
                expect_value();
                break;
            case Phase::CommaOrEnd:
                if (c == ',') {
                    ++pos_;
                    stack_.back().phase = Phase::Key;
                } else if (c == '}') {
                    ++pos_;
                    stack_.pop_back();
                } else fail_here("expected ',' or '}' in JSON object");
                break;
            default:
                fail_here("internal error: bad object phase");
            }
        }
    }

public:
    void expect_top_level_value() {
        expect_value();
        loop_members();
        skip_ws();
        if (pos_ != text_.size()) fail_here("trailing content after JSON value");
    }
};

void validate_json(std::string_view text, std::size_t max_depth) {
    Validator validator(text, max_depth);
    validator.expect_top_level_value();
}

// ---------------------------------------------------------------------------
// Tree conversion (kson tree -> Value), enforcing the data contract.
// ---------------------------------------------------------------------------
struct Converter {
    const Limits &limits;
    std::size_t context_bytes = 0;

    void charge(std::size_t count) {
        const auto max = limits.max_context_bytes;
        if (count > max || context_bytes > max - count)
            throw std::runtime_error("JSON: context byte budget exceeded");
        context_bytes += count;
    }

    Value scalar(const kson_node_t *node) const {
        const std::string_view text(node->v.str);
        if (text == "true") return Value(true);
        if (text == "false") return Value(false);
        if (text == "null") return Value(nullptr);
        // Number: integers that fit stay int64_t; everything else is double.
        const bool integral = (text[0] == '-' ? is_digit(text[1]) : is_digit(text[0])) &&
                              text.find('.') == std::string_view::npos &&
                              text.find('e') == std::string_view::npos &&
                              text.find('E') == std::string_view::npos;
        if (integral) {
            errno = 0;
            char *end = nullptr;
            const long long parsed = std::strtoll(text.data(), &end, 10);
            if (errno == 0 && end && *end == '\0')
                return Value(static_cast<std::int64_t>(parsed));
        }
        errno = 0;
        char *end = nullptr;
        const double parsed = std::strtod(text.data(), &end);
        if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(static_cast<double>(parsed)))
            throw std::runtime_error("JSON: number out of range");
        return Value(parsed);
    }

    Value convert(const kson_node_t *node, std::size_t depth) {
        if (depth > limits.max_value_depth)
            throw std::runtime_error("JSON: nesting exceeds contract depth");
        switch (node->type) {
        case KSON_TYPE_BRACE: {
            Value::Object object;
            for (long i = 0; i < static_cast<long>(node->n); ++i) {
                const kson_node_t *child = node->v.child[i];
                if (!child->key)
                    throw std::runtime_error("JSON: object member without key");
                const std::string key(child->key);
                if (!object.emplace(key, convert(child, depth + 1)).second)
                    throw std::runtime_error("JSON: duplicate object key '" + key + "'");
                charge(key.size());
            }
            return Value(std::move(object));
        }
        case KSON_TYPE_BRACKET: {
            Value::Array array;
            array.reserve(node->n);
            for (long i = 0; i < static_cast<long>(node->n); ++i)
                array.push_back(convert(node->v.child[i], depth + 1));
            return Value(std::move(array));
        }
        case KSON_TYPE_DBL_QUOTE: {
            std::string text = decode_string(node->v.str, node->v.str, 0);
            charge(text.size());
            return Value(std::move(text));
        }
        case KSON_TYPE_NO_QUOTE:
            return scalar(node);
        case KSON_TYPE_SGL_QUOTE:
        default:
            throw std::runtime_error("JSON: non-standard value rejected");
        }
    }
};

} // namespace

Context load_json_context(const std::filesystem::path &file, const Limits &limits) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read JSON data file: " + file.string());
    std::string text;
    char chunk[8192];
    const auto cap = limits.max_context_bytes;
    while (in.read(chunk, sizeof chunk) || in.gcount()) {
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n > cap || text.size() > cap - n)
            throw std::runtime_error("JSON data file exceeds context size limit: " + file.string());
        text.append(chunk, n);
    }
    if (in.bad()) throw std::runtime_error("JSON data I/O error: " + file.string());

    validate_json(text, limits.max_value_depth);

    kson_t *tree = kson_parse(text.c_str());
    if (!tree || !tree->root)
        throw std::runtime_error("JSON: parser rejected document: " + file.string());
    struct TreeGuard {
        kson_t *tree;
        ~TreeGuard() { kson_destroy(tree); }
    } guard{tree};

    const kson_node_t *root = tree->root;
    if (root->type != KSON_TYPE_BRACE)
        throw std::runtime_error("JSON: document root must be an object");
    Converter converter{limits, 0};
    Value value = converter.convert(root, 0);
    return std::move(std::get<Value::Object>(value.data));
}

} // namespace moonstone::qmr::cli
