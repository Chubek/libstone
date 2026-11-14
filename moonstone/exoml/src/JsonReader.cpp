#include "JsonReader.hpp"
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace moonstone::exoml::contract::detail {
namespace {

struct Parser {
    std::string_view input;
    std::size_t pos = 0;
    unsigned depth = 0;
    static constexpr unsigned max_depth = 64;
    static constexpr std::size_t max_string_bytes = 1024 * 1024;
    static constexpr std::size_t max_container = 65536;

    [[noreturn]] void fail(const char *message) {
        throw std::invalid_argument(std::string("JSON parse error at position ") +
                                    std::to_string(pos) + ": " + message);
    }
    char peek() {
        if (pos >= input.size())
            fail("unexpected end of input");
        return input[pos];
    }
    char advance() {
        char c = peek();
        ++pos;
        return c;
    }
    void skip_ws() {
        while (pos < input.size()) {
            char c = input[pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++pos;
        }
    }
    void expect(char c) {
        skip_ws();
        if (advance() != c) {
            std::string msg = "expected '";
            msg += c;
            msg += "'";
            fail(msg.c_str());
        }
    }
    std::string parse_string_content() {
        std::string result;
        while (true) {
            if (pos >= input.size())
                fail("unterminated string");
            if (result.size() > max_string_bytes)
                fail("string exceeds budget");
            char c = advance();
            if (c == '"')
                return result;
            if (c == '\\') {
                if (pos >= input.size())
                    fail("unterminated escape");
                char esc = advance();
                switch (esc) {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                case 'u': {
                    if (pos + 4 > input.size())
                        fail("incomplete unicode escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = advance();
                        code <<= 4;
                        if (h >= '0' && h <= '9')
                            code |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            code |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            code |= unsigned(h - 'A' + 10);
                        else
                            fail("invalid unicode hex digit");
                    }
                    if (code < 0x80) {
                        result += char(code);
                    } else if (code < 0x800) {
                        result += char(0xC0 | (code >> 6));
                        result += char(0x80 | (code & 0x3F));
                    } else {
                        result += char(0xE0 | (code >> 12));
                        result += char(0x80 | ((code >> 6) & 0x3F));
                        result += char(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default:
                    fail("invalid escape sequence");
                }
            } else {
                result += c;
            }
        }
    }
    JsonValue parse_value() {
        skip_ws();
        if (++depth > max_depth)
            fail("nesting depth exceeded");
        JsonValue result;
        char c = peek();
        if (c == '"') {
            advance();
            result.type = JsonType::String;
            result.string_value = parse_string_content();
        } else if (c == '{') {
            advance();
            result.type = JsonType::Object;
            skip_ws();
            if (pos < input.size() && input[pos] == '}') {
                advance();
                --depth;
                return result;
            }
            while (true) {
                skip_ws();
                if (advance() != '"')
                    fail("expected object key string");
                std::string key = parse_string_content();
                skip_ws();
                expect(':');
                JsonValue val = parse_value();
                if (result.object_value.size() >= max_container)
                    fail("object exceeds budget");
                result.object_value.emplace_back(std::move(key), std::move(val));
                skip_ws();
                char sep = advance();
                if (sep == '}')
                    break;
                if (sep != ',')
                    fail("expected ',' or '}' in object");
            }
        } else if (c == '[') {
            advance();
            result.type = JsonType::Array;
            skip_ws();
            if (pos < input.size() && input[pos] == ']') {
                advance();
                --depth;
                return result;
            }
            while (true) {
                JsonValue val = parse_value();
                if (result.array_value.size() >= max_container)
                    fail("array exceeds budget");
                result.array_value.push_back(std::move(val));
                skip_ws();
                char sep = advance();
                if (sep == ']')
                    break;
                if (sep != ',')
                    fail("expected ',' or ']' in array");
            }
        } else if (c == 't') {
            if (pos + 4 > input.size() || input.substr(pos, 4) != "true")
                fail("invalid literal");
            pos += 4;
            result.type = JsonType::Bool;
            result.bool_value = true;
        } else if (c == 'f') {
            if (pos + 5 > input.size() || input.substr(pos, 5) != "false")
                fail("invalid literal");
            pos += 5;
            result.type = JsonType::Bool;
            result.bool_value = false;
        } else if (c == 'n') {
            if (pos + 4 > input.size() || input.substr(pos, 4) != "null")
                fail("invalid literal");
            pos += 4;
            result.type = JsonType::Null;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            std::size_t start = pos;
            bool is_float = false;
            if (c == '-')
                ++pos;
            if (pos >= input.size() || input[pos] < '0' || input[pos] > '9')
                fail("invalid number");
            if (input[pos] == '0') {
                ++pos;
            } else {
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9')
                    ++pos;
            }
            if (pos < input.size() && input[pos] == '.') {
                is_float = true;
                ++pos;
                if (pos >= input.size() || input[pos] < '0' || input[pos] > '9')
                    fail("invalid number fraction");
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9')
                    ++pos;
            }
            if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
                is_float = true;
                ++pos;
                if (pos < input.size() && (input[pos] == '+' || input[pos] == '-'))
                    ++pos;
                if (pos >= input.size() || input[pos] < '0' || input[pos] > '9')
                    fail("invalid number exponent");
                while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9')
                    ++pos;
            }
            std::string_view num = input.substr(start, pos - start);
            if (is_float) {
                result.type = JsonType::Double;
                result.double_value = std::stod(std::string(num));
                if (!std::isfinite(result.double_value))
                    fail("non-finite number");
            } else {
                result.type = JsonType::Int;
                auto [end, err] =
                    std::from_chars(num.data(), num.data() + num.size(), result.int_value);
                if (err != std::errc{} || end != num.data() + num.size())
                    fail("integer out of range");
            }
        } else {
            fail("unexpected character");
        }
        --depth;
        return result;
    }
};

static const JsonValue null_value;

} // namespace

const JsonValue &JsonValue::operator[](std::string_view key) const {
    if (type != JsonType::Object)
        return null_value;
    for (const auto &[k, v] : object_value)
        if (k == key)
            return v;
    return null_value;
}
const JsonValue &JsonValue::operator[](std::size_t index) const {
    if (type != JsonType::Array || index >= array_value.size())
        return null_value;
    return array_value[index];
}
bool JsonValue::contains(std::string_view key) const {
    if (type != JsonType::Object)
        return false;
    for (const auto &[k, v] : object_value)
        if (k == key)
            return true;
    return false;
}
std::size_t JsonValue::size() const {
    if (type == JsonType::Array)
        return array_value.size();
    if (type == JsonType::Object)
        return object_value.size();
    return 0;
}
std::string JsonValue::as_string() const {
    if (type != JsonType::String)
        throw std::invalid_argument("expected string JSON value");
    return string_value;
}
std::int64_t JsonValue::as_int() const {
    if (type != JsonType::Int)
        throw std::invalid_argument("expected integer JSON value");
    return int_value;
}
bool JsonValue::as_bool() const {
    if (type != JsonType::Bool)
        throw std::invalid_argument("expected boolean JSON value");
    return bool_value;
}
double JsonValue::as_number() const {
    if (type == JsonType::Int)
        return static_cast<double>(int_value);
    if (type == JsonType::Double)
        return double_value;
    throw std::invalid_argument("expected numeric JSON value");
}

JsonValue parse_json(std::string_view input) {
    Parser parser{input, 0, 0};
    JsonValue result = parser.parse_value();
    parser.skip_ws();
    if (parser.pos != parser.input.size())
        parser.fail("trailing content after JSON value");
    return result;
}

} // namespace moonstone::exoml::contract::detail
