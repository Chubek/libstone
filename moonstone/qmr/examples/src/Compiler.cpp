#include "qmr/Qmr.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace moonstone::qmr {

Error::Error(std::string code, std::string file, std::size_t line, std::size_t column,
             std::string message)
    : std::runtime_error(file + ":" + std::to_string(line) + ":" +
                         std::to_string(column) + ": " + code + ": " + message),
      code_(std::move(code)), file_(std::move(file)), line_(line), column_(column) {}

namespace {
bool starts_at(std::string_view s, std::size_t n, std::string_view needle) {
    return n <= s.size() && s.substr(n, needle.size()) == needle;
}
bool white(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
std::string_view trim(std::string_view s) {
    while (!s.empty() && white(s.front())) s.remove_prefix(1);
    while (!s.empty() && white(s.back())) s.remove_suffix(1);
    return s;
}
// A closer inside a Lua string or long bracket is data, not a QMR delimiter.
std::size_t find_close(std::string_view src, std::size_t from,
                       std::string_view closer, bool is_comment) {
    if (is_comment) return src.find(closer, from);
    for (std::size_t i = from; i < src.size(); ++i) {
        if (src.substr(i, closer.size()) == closer) return i;
        if (src[i] == '\'' || src[i] == '"') {
            char quote = src[i++];
            for (; i < src.size(); ++i) {
                if (src[i] == '\\') { if (i + 1 < src.size()) ++i; }
                else if (src[i] == quote) break;
            }
        } else if (src[i] == '[') {
            std::size_t j = i + 1;
            while (j < src.size() && src[j] == '=') ++j;
            if (j >= src.size() || src[j] != '[') continue;
            std::string end = "]" + std::string(j - i - 1, '=') + "]";
            const auto finish = src.find(end, j + 1);
            if (finish == std::string_view::npos) return src.size();
            i = finish + end.size() - 1;
        }
    }
    return std::string_view::npos;
}
std::string lua_string(std::string_view s) {
    std::string out = "\"";
    out.reserve(s.size() + s.size() / 8 + 2);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32 || c == 127) {
            out += "\\";
            out += static_cast<char>('0' + c / 100);
            out += static_cast<char>('0' + c / 10 % 10);
            out += static_cast<char>('0' + c % 10);
        } else out += static_cast<char>(c);
    }
    out += '"';
    return out;
}
} // namespace

Program compile(std::string_view src, std::string name, const Limits &limits) {
    if (name.empty()) name = "<template>";
    if (src.size() > limits.max_template_bytes)
        throw Error("QMR_SIZE", name, 1, 1, "template exceeds max_template_bytes");
    if (limits.max_nodes == 0 || limits.max_compiled_bytes == 0)
        throw Error("QMR_LIMIT", name, 1, 1, "zero compilation limit");
    std::vector<std::size_t> newlines;
    for (std::size_t i = 0; i < src.size(); ++i)
        if (src[i] == '\n') newlines.push_back(i);
    auto fail = [&](std::size_t pos, const char *code, std::string message) -> void {
        auto it = std::lower_bound(newlines.begin(), newlines.end(), pos);
        std::size_t line = static_cast<std::size_t>(it - newlines.begin()) + 1;
        std::size_t col = pos - (it == newlines.begin() ? 0 : *(it - 1) + 1) + 1;
        throw Error(code, name, line, col, std::move(message));
    };
    Program p;
    p.name = name;
    p.lua_source = "-- QMR template; generated as Lua source, not bytecode\n"
                   "local function __qmr_emit(value, do_escape)\n"
                   "  local ok, err = __qmr_emit_native(value, do_escape)\n"
                   "  if not ok then error(err, 0) end\n"
                   "end\n"
                   "local function __qmr_write(text)\n"
                   "  local ok, err = __qmr_write_native(text)\n"
                   "  if not ok then error(err, 0) end\n"
                   "end\n"
                   "local function __qmr_include(path, vars)\n"
                   "  local ok, err = __qmr_include_native(path, vars)\n"
                   "  if not ok then error(err, 0) end\n"
                   "end\n";
    auto append = [&](std::size_t pos, std::string_view s) {
        if (s.size() > limits.max_compiled_bytes ||
            p.lua_source.size() > limits.max_compiled_bytes - s.size())
            fail(pos, "QMR_COMPILED_SIZE", "compiled template exceeds limit");
        p.lua_source.append(s);
    };
    auto literal = [&](std::string_view text, std::size_t pos) {
        if (text.empty()) return;
        if (++p.nodes > limits.max_nodes) fail(pos, "QMR_NODES", "too many template nodes");
        append(pos, "__qmr_write(");
        append(pos, lua_string(text));
        append(pos, ")\n");
    };
    std::size_t pos = 0;
    bool strip_next = false;
    while (pos < src.size()) {
        std::size_t begin = src.find('{', pos);
        if (begin == std::string_view::npos) begin = src.size();
        std::size_t open = begin;
        std::string_view close;
        enum { Text, Expr, Raw, Code, Comment } kind = Text;
        while (open < src.size()) {
            if (starts_at(src, open, "{{{")) { kind = Raw; close = "}}}"; break; }
            if (starts_at(src, open, "{{")) { kind = Expr; close = "}}"; break; }
            if (starts_at(src, open, "{%")) { kind = Code; close = "%}"; break; }
            if (starts_at(src, open, "{#")) { kind = Comment; close = "#}"; break; }
            open = src.find('{', open + 1);
            if (open == std::string_view::npos) open = src.size();
        }
        auto text = src.substr(pos, open - pos);
        if (strip_next) {
            while (!text.empty() && white(text.front())) text.remove_prefix(1);
            strip_next = false;
        }
        const std::size_t open_len = kind == Raw ? 3 : 2;
        bool trim_left = kind != Text && open + open_len < src.size() && src[open + open_len] == '-';
        if (trim_left) while (!text.empty() && white(text.back())) text.remove_suffix(1);
        literal(text, pos);
        if (kind == Text) break;
        std::size_t body_start = open + open_len + (trim_left ? 1 : 0);
        auto end = find_close(src, body_start, close, kind == Comment);
        if (end == std::string_view::npos)
            fail(open, "QMR_UNCLOSED", "unclosed template delimiter");
        bool trim_right = end > body_start && src[end - 1] == '-';
        auto body = trim(src.substr(body_start, end - body_start - (trim_right ? 1 : 0)));
        if (++p.nodes > limits.max_nodes) fail(open, "QMR_NODES", "too many template nodes");
        if ((kind == Expr || kind == Raw || kind == Code) && body.empty())
            fail(open, "QMR_EMPTY", "empty expression or statement tag");
        if (kind == Expr || kind == Raw) {
            append(open, "__qmr_emit((");
            append(open, body);
            append(open, kind == Expr ? "), true)\n" : "), false)\n");
        } else if (kind == Code) {
            if (body.substr(0, 7) == "include" &&
                (body.size() == 7 || white(body[7]))) {
                auto include_expr = trim(body.substr(7));
                if (include_expr.empty()) fail(open, "QMR_INCLUDE", "include requires a Lua expression");
                append(open, "__qmr_include(");
                append(open, include_expr);
                append(open, ")\n");
            } else {
                append(open, body);
                append(open, "\n");
            }
        }
        pos = end + close.size();
        strip_next = trim_right;
    }
    return p;
}

std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string escape_url(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%'; out += hex[c >> 4]; out += hex[c & 15];
        }
    }
    return out;
}

} // namespace moonstone::qmr
