#include "Internal.hpp"

namespace gateml {
Error::Error(Location loc, std::string message)
    : std::runtime_error(std::move(message)), location(std::move(loc)) {}

std::string preprocess(std::string_view source, const std::string& file) {
    std::string out(source);
    std::size_t line = 1, column = 1, depth = 0;
    bool quoted = false, escaped = false, comment = false;
    Location start{file, 1, 1};
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i], n = i + 1 < source.size() ? source[i + 1] : '\0';
        if (depth) {
            if (c == '(' && n == '*') { ++depth; out[i] = out[i + 1] = ' '; ++i; ++column; }
            else if (c == '*' && n == ')') { --depth; out[i] = out[i + 1] = ' '; ++i; ++column; }
            else if (c != '\n') out[i] = ' ';
        } else if (comment) {
            if (c == '\n') comment = false;
            else out[i] = ' ';
        } else if (quoted) {
            if (!escaped && c == '"') quoted = false;
            if (!escaped && c == '\\') escaped = true;
            else escaped = false;
        } else if (c == '"') quoted = true;
        else if (c == '(' && n == '*' && !(i + 2 < source.size() && source[i + 2] == ')')) {
            // Outside comments, exact `(*)` names multiplication. All other
            // `(*` sequences open comments, even immediately after `(`.
            start = {file, line, column}; depth = 1;
            out[i] = out[i + 1] = ' '; ++i; ++column;
        } else if (c == '/' && n == '/') { comment = true; out[i] = ' '; }
        if (c == '\n') { ++line; column = 1; } else ++column;
    }
    if (depth) throw Error(start, "unterminated comment");
    return out;
}
} // namespace gateml
