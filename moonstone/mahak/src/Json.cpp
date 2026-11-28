#include "mahak/Json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <system_error>

namespace moonstone::mahak {
inline namespace v1 {
namespace {

Error invalid(std::string message, std::string_view source = {}) {
    Error error;
    error.category = ErrorCategory::InvalidInput;
    if (!source.empty())
        message = std::string(source) + ": " + message;
    error.message = std::move(message);
    return error;
}

void append_utf8(std::string &out, std::uint32_t code) {
    if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

class Parser {
  public:
    Parser(std::string_view text, std::string_view source) : text_(text), source_(source) {}

    Result<Json> parse() {
        skip_whitespace();
        auto value = parse_value(0);
        if (!value)
            return value.error();
        skip_whitespace();
        if (position_ != text_.size())
            return fail("unexpected trailing characters");
        return std::move(value.value());
    }

  private:
    std::string_view text_;
    std::string_view source_;
    std::size_t position_ = 0;
    std::size_t nodes_ = 0;

    Error fail(std::string message) const {
        message += " at byte " + std::to_string(position_);
        return invalid(std::move(message), source_);
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    bool consume_literal(std::string_view literal) {
        if (text_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    Result<Json> parse_value(std::size_t depth) {
        if (depth > 128)
            return fail("JSON nesting exceeds 128 levels");
        if (++nodes_ > 1000000)
            return fail("JSON node budget exceeded");
        skip_whitespace();
        if (position_ >= text_.size())
            return fail("unexpected end of JSON input");
        switch (text_[position_]) {
        case 'n':
            if (!consume_literal("null"))
                return fail("invalid literal; expected null");
            return Json();
        case 't':
            if (!consume_literal("true"))
                return fail("invalid literal; expected true");
            return Json(true);
        case 'f':
            if (!consume_literal("false"))
                return fail("invalid literal; expected false");
            return Json(false);
        case '"':
            return parse_string();
        case '[':
            return parse_array(depth);
        case '{':
            return parse_object(depth);
        default:
            if (text_[position_] == '-' || (text_[position_] >= '0' && text_[position_] <= '9'))
                return parse_number();
            return fail("unexpected character");
        }
    }

    Result<Json> parse_string() {
        if (!consume('"'))
            return fail("expected string");
        std::string result;
        while (position_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') {
                if (!valid_utf8(result))
                    return fail("string contains invalid UTF-8");
                return Json(std::move(result));
            }
            if (c < 0x20)
                return fail("unescaped control character in string");
            if (c != '\\') {
                result.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= text_.size())
                return fail("unterminated escape sequence");
            const char escape = text_[position_++];
            switch (escape) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                auto code = parse_hex4();
                if (!code)
                    return code.error();
                std::uint32_t value = code.value();
                if (value >= 0xD800 && value <= 0xDBFF) {
                    if (position_ + 1 >= text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1] != 'u')
                        return fail("high surrogate without low surrogate");
                    position_ += 2;
                    auto low = parse_hex4();
                    if (!low)
                        return low.error();
                    if (low.value() < 0xDC00 || low.value() > 0xDFFF)
                        return fail("invalid low surrogate");
                    value = 0x10000 + ((value - 0xD800) << 10) + (low.value() - 0xDC00);
                } else if (value >= 0xDC00 && value <= 0xDFFF) {
                    return fail("unexpected low surrogate");
                }
                append_utf8(result, value);
                break;
            }
            default:
                return fail("invalid escape sequence");
            }
        }
        return fail("unterminated string");
    }

    Result<std::uint32_t> parse_hex4() {
        if (position_ + 4 > text_.size())
            return fail("truncated \\u escape");
        std::uint32_t value = 0;
        for (int i = 0; i != 4; ++i) {
            const int digit = hex_digit(text_[position_++]);
            if (digit < 0)
                return fail("invalid \\u escape");
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    Result<Json> parse_array(std::size_t depth) {
        consume('[');
        Json::Array array;
        skip_whitespace();
        if (consume(']'))
            return Json(std::move(array));
        for (;;) {
            auto value = parse_value(depth + 1);
            if (!value)
                return value.error();
            array.push_back(std::move(value.value()));
            skip_whitespace();
            if (consume(']'))
                break;
            if (!consume(','))
                return fail("expected ',' or ']' in array");
        }
        return Json(std::move(array));
    }

    Result<Json> parse_object(std::size_t depth) {
        consume('{');
        Json::Object object;
        skip_whitespace();
        if (consume('}'))
            return Json(std::move(object));
        for (;;) {
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_] != '"')
                return fail("expected object key string");
            auto key_value = parse_string();
            if (!key_value)
                return key_value.error();
            std::string key = *key_value.value().as_string();
            skip_whitespace();
            if (!consume(':'))
                return fail("expected ':' after object key");
            auto value = parse_value(depth + 1);
            if (!value)
                return value.error();
            if (!object.emplace(std::move(key), std::move(value.value())).second)
                return fail("duplicate object key");
            skip_whitespace();
            if (consume('}'))
                break;
            if (!consume(','))
                return fail("expected ',' or '}' in object");
        }
        return Json(std::move(object));
    }

    Result<Json> parse_number() {
        const std::size_t start = position_;
        if (consume('-')) {
            if (position_ >= text_.size())
                return fail("truncated number");
        }
        if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9')
            return fail("expected digit");
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
                return fail("leading zero in number");
        } else {
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9')
                ++position_;
        }
        bool integral = true;
        if (consume('.')) {
            integral = false;
            if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9')
                return fail("expected fraction digit");
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9')
                ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            integral = false;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9')
                return fail("expected exponent digit");
            while (position_ < text_.size() && text_[position_] >= '0' &&
                   text_[position_] <= '9')
                ++position_;
        }
        const std::string_view token = text_.substr(start, position_ - start);
        if (integral) {
            std::int64_t integer = 0;
            const auto result =
                std::from_chars(token.data(), token.data() + token.size(), integer);
            if (result.ec == std::errc() && result.ptr == token.data() + token.size())
                return Json(integer);
        }
        double number = 0.0;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), number,
                                            std::chars_format::general);
        if (result.ec != std::errc() || result.ptr != token.data() + token.size() ||
            !std::isfinite(number))
            return fail("number is out of range");
        return Json(number);
    }
};

class JsonWriter {
  public:
    JsonWriter(bool pretty, unsigned indent) : pretty_(pretty), indent_(indent) {}

    bool write(const Json &value) {
        write_value(value, 0);
        return ok_;
    }

    const std::string &bytes() const { return bytes_; }

  private:
    bool pretty_ = false;
    unsigned indent_ = 2;
    bool ok_ = true;
    std::string bytes_;

    void newline(unsigned depth) {
        if (!pretty_)
            return;
        bytes_.push_back('\n');
        bytes_.append(static_cast<std::size_t>(depth) * indent_, ' ');
    }

    void write_string(std::string_view value) {
        static const char hex[] = "0123456789abcdef";
        bytes_.push_back('"');
        for (unsigned char c : value) {
            switch (c) {
            case '"':
                bytes_ += "\\\"";
                break;
            case '\\':
                bytes_ += "\\\\";
                break;
            case '\b':
                bytes_ += "\\b";
                break;
            case '\f':
                bytes_ += "\\f";
                break;
            case '\n':
                bytes_ += "\\n";
                break;
            case '\r':
                bytes_ += "\\r";
                break;
            case '\t':
                bytes_ += "\\t";
                break;
            default:
                if (c < 0x20) {
                    bytes_ += "\\u00";
                    bytes_.push_back(hex[c >> 4]);
                    bytes_.push_back(hex[c & 15]);
                } else {
                    bytes_.push_back(static_cast<char>(c));
                }
            }
        }
        bytes_.push_back('"');
    }

    void write_number(double value) {
        if (!std::isfinite(value)) {
            ok_ = false;
            return;
        }
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                          std::chars_format::general);
        if (result.ec != std::errc()) {
            ok_ = false;
            return;
        }
        std::string_view text(buffer, static_cast<std::size_t>(result.ptr - buffer));
        bytes_.append(text);
        if (text.find_first_of(".eE") == std::string_view::npos)
            bytes_ += ".0";
    }

    void write_value(const Json &value, unsigned depth) {
        if (!ok_)
            return;
        switch (value.type()) {
        case Json::Type::Null:
            bytes_ += "null";
            break;
        case Json::Type::Boolean:
            bytes_ += value.as_bool().value() ? "true" : "false";
            break;
        case Json::Type::Integer:
            bytes_ += std::to_string(value.as_int64().value());
            break;
        case Json::Type::Number:
            write_number(value.as_double().value());
            break;
        case Json::Type::String:
            write_string(*value.as_string());
            break;
        case Json::Type::Array: {
            const auto &array = *value.as_array();
            bytes_.push_back('[');
            for (std::size_t index = 0; index != array.size(); ++index) {
                if (index != 0)
                    bytes_.push_back(',');
                newline(depth + 1);
                write_value(array[index], depth + 1);
            }
            if (!array.empty())
                newline(depth);
            bytes_.push_back(']');
            break;
        }
        case Json::Type::Object: {
            const auto &object = *value.as_object();
            bytes_.push_back('{');
            bool first = true;
            for (const auto &[key, item] : object) {
                if (!first)
                    bytes_.push_back(',');
                first = false;
                newline(depth + 1);
                write_string(key);
                bytes_ += pretty_ ? ": " : ":";
                write_value(item, depth + 1);
            }
            if (!object.empty())
                newline(depth);
            bytes_.push_back('}');
            break;
        }
        }
    }
};

} // namespace

std::optional<bool> Json::as_bool() const {
    if (type_ != Type::Boolean)
        return std::nullopt;
    return boolean_;
}

std::optional<std::int64_t> Json::as_int64() const {
    if (type_ == Type::Integer)
        return integer_;
    if (type_ == Type::Number && number_ >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        number_ <= static_cast<double>(std::numeric_limits<std::int64_t>::max()) &&
        std::floor(number_) == number_)
        return static_cast<std::int64_t>(number_);
    return std::nullopt;
}

std::optional<std::uint64_t> Json::as_uint64() const {
    if (type_ == Type::Integer && integer_ >= 0)
        return static_cast<std::uint64_t>(integer_);
    if (type_ == Type::Number && std::isfinite(number_) && number_ >= 0.0 &&
        number_ <= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) &&
        std::floor(number_) == number_)
        return static_cast<std::uint64_t>(number_);
    return std::nullopt;
}

std::optional<double> Json::as_double() const {
    if (type_ == Type::Integer)
        return static_cast<double>(integer_);
    if (type_ == Type::Number)
        return number_;
    return std::nullopt;
}

const std::string *Json::as_string() const noexcept {
    return type_ == Type::String ? &string_ : nullptr;
}
const Json::Array *Json::as_array() const noexcept {
    return type_ == Type::Array ? &array_ : nullptr;
}
const Json::Object *Json::as_object() const noexcept {
    return type_ == Type::Object ? &object_ : nullptr;
}
Json::Array *Json::as_array() noexcept { return type_ == Type::Array ? &array_ : nullptr; }
Json::Object *Json::as_object() noexcept { return type_ == Type::Object ? &object_ : nullptr; }

Json &Json::set(std::string key, Json value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
        array_.clear();
        string_.clear();
    }
    object_[std::move(key)] = std::move(value);
    return *this;
}

Json &Json::push(Json value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
        object_.clear();
        string_.clear();
    }
    array_.push_back(std::move(value));
    return *this;
}

const Json *Json::find(std::string_view key) const noexcept {
    if (type_ != Type::Object)
        return nullptr;
    const auto found = object_.find(key);
    return found == object_.end() ? nullptr : &found->second;
}

Json *Json::find(std::string_view key) noexcept {
    if (type_ != Type::Object)
        return nullptr;
    const auto found = object_.find(key);
    return found == object_.end() ? nullptr : &found->second;
}

std::string Json::dump(bool pretty, unsigned indent, bool *ok) const {
    JsonWriter writer(pretty, indent);
    const bool success = writer.write(*this);
    if (ok)
        *ok = success;
    return success ? writer.bytes() : std::string{};
}

Result<Json> Json::parse(std::string_view text, std::string_view source) {
    return Parser(text, source).parse();
}

bool operator==(const Json &a, const Json &b) noexcept {
    if (a.type_ != b.type_)
        return false;
    switch (a.type_) {
    case Json::Type::Null:
        return true;
    case Json::Type::Boolean:
        return a.boolean_ == b.boolean_;
    case Json::Type::Integer:
        return a.integer_ == b.integer_;
    case Json::Type::Number:
        return a.number_ == b.number_;
    case Json::Type::String:
        return a.string_ == b.string_;
    case Json::Type::Array:
        return a.array_ == b.array_;
    case Json::Type::Object:
        return a.object_ == b.object_;
    }
    return false;
}

Result<bool> require_bool(const Json &value, std::string_view path) {
    if (const auto result = value.as_bool())
        return *result;
    return invalid(std::string(path) + ": expected boolean");
}
Result<std::int64_t> require_int64(const Json &value, std::string_view path) {
    if (const auto result = value.as_int64())
        return *result;
    return invalid(std::string(path) + ": expected integer");
}
Result<std::uint64_t> require_uint64(const Json &value, std::string_view path) {
    if (const auto result = value.as_uint64())
        return *result;
    return invalid(std::string(path) + ": expected non-negative integer");
}
Result<double> require_double(const Json &value, std::string_view path) {
    if (const auto result = value.as_double())
        return *result;
    return invalid(std::string(path) + ": expected number");
}
Result<std::string> require_string(const Json &value, std::string_view path) {
    if (const auto *result = value.as_string())
        return *result;
    return invalid(std::string(path) + ": expected string");
}
Result<const Json::Array *> require_array(const Json &value, std::string_view path) {
    if (const auto *result = value.as_array())
        return result;
    return invalid(std::string(path) + ": expected array");
}
Result<const Json::Object *> require_object(const Json &value, std::string_view path) {
    if (const auto *result = value.as_object())
        return result;
    return invalid(std::string(path) + ": expected object");
}

bool valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead <= 0x7F) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t code = 0;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code = lead & 0x07;
        } else {
            return false;
        }
        if (index + length > text.size())
            return false;
        for (std::size_t i = 1; i < length; ++i) {
            const unsigned char continuation = static_cast<unsigned char>(text[index + i]);
            if ((continuation & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (continuation & 0x3F);
        }
        if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
            (length == 4 && code < 0x10000) || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF))
            return false;
        index += length;
    }
    return true;
}

} // namespace v1
} // namespace moonstone::mahak
