#include "Core.hpp"

namespace moonstone::inline v1 {

std::string json_escape(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 32) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::string format(const Diagnostic &d) {
    std::ostringstream out;
    out << d.primary.file << ':' << d.primary.line << ':' << d.primary.column << ": "
        << name(d.severity) << '[' << d.id << "]: " << d.message;
    for (const auto &note : d.notes)
        out << "\n  note: " << note;
    return out.str();
}

std::optional<std::string> apply_edits(const std::string &source, std::vector<SourceEdit> edits) {
    std::sort(edits.begin(), edits.end(),
              [](const auto &a, const auto &b) { return a.begin < b.begin; });
    std::size_t cursor = 0;
    std::string result;
    std::optional<std::size_t> previous_begin;
    for (const auto &edit : edits) {
        // Equal starts (including insertions) have ambiguous ordering. Reject them.
        if (edit.begin < cursor || edit.end < edit.begin || edit.end > source.size() ||
            previous_begin == edit.begin)
            return std::nullopt;
        result.append(source, cursor, edit.begin - cursor);
        result += edit.replacement;
        cursor = edit.end;
        previous_begin = edit.begin;
    }
    result.append(source, cursor, std::string::npos);
    return result;
}

} // namespace moonstone::inline v1
