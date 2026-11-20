#include <cephyr/diagnostics.hpp>

#include <syren/Syren.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ostream>
#include <utility>

namespace cephyr {
namespace {

using MoonstoneDiagnostic = moonstone::Diagnostic;
using MoonstoneDiagnostics = moonstone::Diagnostics;
using MoonstoneSeverity = moonstone::Severity;

std::uint32_t coordinate(unsigned value) noexcept {
    if constexpr (std::numeric_limits<unsigned>::max() >
                  std::numeric_limits<std::uint32_t>::max()) {
        if (value > std::numeric_limits<std::uint32_t>::max())
            return 0;
    }
    return static_cast<std::uint32_t>(value);
}

bool ascii_alpha(unsigned char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool ascii_digit(unsigned char value) noexcept {
    return value >= '0' && value <= '9';
}

bool hex_digit(unsigned char value) noexcept {
    return ascii_digit(value) || (value >= 'A' && value <= 'F') ||
           (value >= 'a' && value <= 'f');
}

bool path_character(unsigned char value) noexcept {
    return ascii_alpha(value) || ascii_digit(value) || value == '-' || value == '.' ||
           value == '_' || value == '~' || value == '/';
}

bool uri_character(unsigned char value) noexcept {
    return path_character(value) || value == ':' || value == '@' || value == '!' ||
           value == '$' || value == '&' || value == '\'' || value == '(' ||
           value == ')' || value == '*' || value == '+' || value == ',' ||
           value == ';' || value == '=' || value == '?' || value == '#' ||
           value == '[' || value == ']';
}

bool explicit_uri(std::string_view value) noexcept {
    if (value.empty() || !ascii_alpha(static_cast<unsigned char>(value.front())))
        return false;
    for (std::size_t i = 1; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == ':')
            return value.substr(i + 1).starts_with("//");
        if (!ascii_alpha(c) && !ascii_digit(c) && c != '+' && c != '-' && c != '.')
            return false;
    }
    return false;
}

bool single_line(std::string_view value) noexcept {
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
    });
}

constexpr char hex[] = "0123456789ABCDEF";

void append_byte_escape(std::string &target, unsigned char value) {
    target += "\\x";
    target.push_back(hex[value >> 4]);
    target.push_back(hex[value & 0x0f]);
}

std::string text_escape(std::string_view value) {
    std::string result;
    for (const auto character : value) {
        const auto c = static_cast<unsigned char>(character);
        switch (c) {
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (c < 0x20 || c == 0x7f)
                append_byte_escape(result, c);
            else
                result.push_back(static_cast<char>(c));
        }
    }
    return result;
}

// Linux arguments and subtool output can contain arbitrary bytes. Preserve valid
// UTF-8 and represent invalid bytes visibly rather than emitting malformed JSON.
std::string sarif_text(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 0x80) {
            result.push_back(value[i++]);
            continue;
        }
        const std::size_t width = c >= 0xc2 && c <= 0xdf ? 2 :
                                  c >= 0xe0 && c <= 0xef ? 3 :
                                  c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        bool valid = width != 0 && width <= value.size() - i;
        for (std::size_t j = 1; valid && j < width; ++j) {
            const auto next = static_cast<unsigned char>(value[i + j]);
            valid = next >= 0x80 && next <= 0xbf;
            if (j == 1) {
                // Reject overlong encodings, surrogates, and values above U+10FFFF.
                valid = valid && (c != 0xe0 || next >= 0xa0) &&
                        (c != 0xed || next <= 0x9f) && (c != 0xf0 || next >= 0x90) &&
                        (c != 0xf4 || next <= 0x8f);
            }
        }
        if (valid) {
            result.append(value.substr(i, width));
            i += width;
        } else {
            append_byte_escape(result, c);
            ++i;
        }
    }
    return result;
}

std::string encode_uri(std::string_view file) {
    std::string path(file);
    const bool windows_absolute =
        path.size() >= 3 && ascii_alpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && (path[2] == '/' || path[2] == '\\');
    if (windows_absolute)
        std::replace(path.begin(), path.end(), '\\', '/');

    const bool absolute = !path.empty() && path.front() == '/';
    const bool is_uri = !windows_absolute && explicit_uri(path);

    std::string result;
    if (windows_absolute)
        result = "file:///";
    else if (absolute && !is_uri)
        result = "file://";

    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto c = static_cast<unsigned char>(path[i]);
        // Raw source paths can contain a literal percent escape, query, or fragment.
        // Keep URI syntax only when the location was supplied as an explicit URI.
        if (is_uri && c == '%' && i + 2 < path.size() &&
            hex_digit(static_cast<unsigned char>(path[i + 1])) &&
            hex_digit(static_cast<unsigned char>(path[i + 2]))) {
            result.append(path, i, 3);
            i += 2;
        } else if ((is_uri && uri_character(c)) || (!is_uri && path_character(c)) ||
                   (windows_absolute && i == 1 && c == ':')) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 0x0f]);
        }
    }
    return result;
}

const char *sarif_level(MoonstoneSeverity severity) noexcept {
    switch (severity) {
    case MoonstoneSeverity::Note:
        return "note";
    case MoonstoneSeverity::Warning:
        return "warning";
    case MoonstoneSeverity::Error:
    case MoonstoneSeverity::Fatal:
        return "error";
    }
    return "error";
}

// Syren's formatter inserts a 1 for missing coordinates and always emits a rule ID.
void render_optional_sarif(std::ostream &out, const MoonstoneDiagnostics &diagnostics) {
    out << R"({"version":"2.1.0","$schema":"https://json.schemastore.org/sarif-2.1.0.json","runs":[{"tool":{"driver":{"name":"Cephyr"}},"results":[)";
    bool first = true;
    for (const auto &diagnostic : diagnostics) {
        if (!first)
            out << ',';
        first = false;
        out << '{';
        if (!diagnostic.id.empty())
            out << R"("ruleId":")" << moonstone::json_escape(diagnostic.id) << "\",";
        out << R"("level":")" << sarif_level(diagnostic.severity)
            << R"(","message":{"text":")" << moonstone::json_escape(diagnostic.message)
            << "\"}";
        if (!diagnostic.primary.file.empty()) {
            out << R"(,"locations":[{"physicalLocation":{"artifactLocation":{"uri":")"
                << moonstone::json_escape(encode_uri(diagnostic.primary.file)) << "\"}";
            if (diagnostic.primary.line != 0) {
                out << R"(,"region":{"startLine":)"
                    << std::to_string(diagnostic.primary.line);
                if (diagnostic.primary.column != 0)
                    out << R"(,"startColumn":)"
                        << std::to_string(diagnostic.primary.column);
                out << '}';
            }
            out << "}}]";
        }
        out << '}';
    }
    out << "]}]}";
}

} // namespace

Diagnostics::Diagnostics(bool warnings_as_errors)
    : warnings_as_errors_(warnings_as_errors) {}

void Diagnostics::emit(Severity severity, std::string_view code, std::string_view message,
                       Location where) {
    entries_.push_back(
        {std::string(code), std::string(message), std::move(where), severity});
}

void Diagnostics::error(std::string_view code, std::string_view message, Location where) {
    emit(Severity::Error, code, message, std::move(where));
}

void Diagnostics::warning(std::string_view code, std::string_view message, Location where) {
    emit(Severity::Warning, code, message, std::move(where));
}

void Diagnostics::note(std::string_view code, std::string_view message, Location where) {
    emit(Severity::Note, code, message, std::move(where));
}

std::size_t Diagnostics::error_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(), [this](const Entry &entry) {
            return entry.severity == Severity::Error ||
                   (warnings_as_errors_ && entry.severity == Severity::Warning);
        }));
}

void Diagnostics::set_warnings_as_errors(bool enabled) noexcept {
    warnings_as_errors_ = enabled;
}

bool Diagnostics::warnings_as_errors() const noexcept {
    return warnings_as_errors_;
}

void Diagnostics::render(std::ostream &out, bool sarif) const {
    MoonstoneDiagnostics diagnostics;
    diagnostics.reserve(entries_.size());
    bool requires_custom_sarif = false;

    for (const auto &entry : entries_) {
        MoonstoneDiagnostic diagnostic;
        diagnostic.id = sarif ? sarif_text(entry.code) : entry.code;
        diagnostic.message = sarif ? sarif_text(entry.message) : entry.message;
        switch (entry.severity) {
        case Severity::Note:
            diagnostic.severity = MoonstoneSeverity::Note;
            break;
        case Severity::Warning:
            diagnostic.severity =
                warnings_as_errors_ ? MoonstoneSeverity::Error : MoonstoneSeverity::Warning;
            break;
        case Severity::Error:
            diagnostic.severity = MoonstoneSeverity::Error;
            break;
        }

        diagnostic.primary.file = entry.where.file;
        diagnostic.primary.line = coordinate(entry.where.line);
        diagnostic.primary.column = coordinate(entry.where.column);
        diagnostic.primary.end_line = diagnostic.primary.line;
        diagnostic.primary.end_column = diagnostic.primary.column;
        requires_custom_sarif =
            requires_custom_sarif || diagnostic.id.empty() ||
            (!entry.where.file.empty() &&
             (diagnostic.primary.line == 0 || diagnostic.primary.column == 0));
        diagnostics.push_back(std::move(diagnostic));
    }

    if (sarif) {
        if (requires_custom_sarif) {
            render_optional_sarif(out, diagnostics);
            return;
        }

        for (auto &diagnostic : diagnostics) {
            if (!diagnostic.primary.file.empty())
                diagnostic.primary.file = encode_uri(diagnostic.primary.file);
        }
        out << moonstone::syren::to_sarif(diagnostics, "Cephyr");
        return;
    }

    moonstone::syren::StderrSink sink(out);
    for (const auto &diagnostic : diagnostics) {
        const bool complete_location = !diagnostic.primary.file.empty() &&
                                       diagnostic.primary.line != 0 &&
                                       diagnostic.primary.column != 0;
        const bool safe_for_syren =
            complete_location && !diagnostic.id.empty() &&
            single_line(diagnostic.primary.file) && single_line(diagnostic.id) &&
            single_line(diagnostic.message);
        if (safe_for_syren) {
            sink.emit(diagnostic);
            continue;
        }

        if (!diagnostic.primary.file.empty()) {
            out << text_escape(diagnostic.primary.file);
            if (diagnostic.primary.line != 0) {
                out << ':' << std::to_string(diagnostic.primary.line);
                if (diagnostic.primary.column != 0)
                    out << ':' << std::to_string(diagnostic.primary.column);
            }
            out << ": ";
        }
        out << moonstone::name(diagnostic.severity);
        if (!diagnostic.id.empty())
            out << '[' << text_escape(diagnostic.id) << ']';
        out << ": " << text_escape(diagnostic.message) << '\n';
    }
}

} // namespace cephyr
