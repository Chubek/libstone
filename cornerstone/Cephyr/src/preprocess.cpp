#include <cephyr/preprocess.hpp>

#include "sdk_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <string_view>
#include <system_error>

#ifndef UCPP_CONFIG
#define UCPP_CONFIG
#endif
extern "C" {
#include "cpp.h"
// The raw scanner, unlike cpp()/lex(), does not execute directives or
// expand macros. These are the internal entry points in ucpp/ucppi.h.
void ucpp_init_cppm(void);
int ucpp_next_token(struct lexer_state *);
char *ucpp_token_name(struct token *);
}

namespace {

using cephyr::Diagnostics;
using cephyr::Location;
using cephyr::Preprocessed;

struct UcppDiagnosticContext {
    Diagnostics *diagnostics = nullptr;
    std::string root_file;
    unsigned errors = 0;
    const Preprocessed *output_map = nullptr;
};

thread_local UcppDiagnosticContext *active_diagnostics = nullptr;

std::mutex &ucpp_mutex() {
    // ucpp intentionally exposes its preprocessing state as globals. Calls
    // are serialized and every call initializes and wipes those globals,
    // giving each translation unit a clean macro/include/file environment.
    static std::mutex mutex;
    return mutex;
}

std::string format_message(const char *format, va_list arguments) {
    va_list copy;
    va_copy(copy, arguments);
    const int length = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    if (length < 0)
        return format ? std::string(format) : std::string{};

    std::string message(static_cast<std::size_t>(length), '\0');
    if (length != 0)
        std::vsnprintf(message.data(), message.size() + 1, format, arguments);
    return message;
}

unsigned line_number(long line) {
    if (line <= 0)
        return 0;
    return static_cast<unsigned>(std::min<unsigned long>(
        static_cast<unsigned long>(line), std::numeric_limits<unsigned>::max()));
}

Location ucpp_location(long line) {
    if (active_diagnostics && active_diagnostics->output_map)
        return active_diagnostics->output_map->locate(line_number(line));
    std::string filename;
    if (current_filename)
        filename = current_filename;
    else if (active_diagnostics)
        filename = active_diagnostics->root_file;
    return {std::move(filename), line_number(line), 0U};
}

void report_ucpp(bool warning, const char *code, long line,
                 const char *format, va_list arguments) {
    if (!active_diagnostics || !active_diagnostics->diagnostics)
        return;

    std::string message = format_message(format, arguments);
    if (warning) {
        active_diagnostics->diagnostics->warning(
            code, std::move(message), ucpp_location(line));
    } else {
        ++active_diagnostics->errors;
        active_diagnostics->diagnostics->error(
            code, std::move(message), ucpp_location(line));
    }
}

std::string quote_directive_filename(std::string_view filename) {
    std::string quoted;
    quoted.reserve(filename.size() + 2);
    quoted.push_back('"');
    for (const unsigned char c : filename) {
        switch (c) {
        case '\\':
        case '"':
            quoted.push_back('\\');
            quoted.push_back(static_cast<char>(c));
            break;
        case '\n':
            quoted += "\\n";
            break;
        case '\r':
            quoted += "\\r";
            break;
        default:
            quoted.push_back(static_cast<char>(c));
            break;
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::filesystem::path
resolve_forced_include(const std::filesystem::path &include,
                       const cephyr::PreprocessOptions &options) {
    if (include.is_absolute())
        return include;

    // Match the useful -include search order: the preprocessor working
    // directory first, then -I and -isystem. In particular, do not let the
    // synthetic directive accidentally search beside the primary source as
    // an ordinary quoted include would.
    std::vector<std::filesystem::path> candidates;
    candidates.reserve(1 + options.include_dirs.size() +
                       options.system_include_dirs.size());
    candidates.push_back(include);
    for (const auto &directory : options.include_dirs)
        candidates.push_back(directory / include);
    for (const auto &directory : options.system_include_dirs)
        candidates.push_back(directory / include);

    std::error_code error;
    for (const auto &candidate : candidates) {
        if (std::filesystem::exists(candidate, error)) {
            auto absolute = std::filesystem::absolute(candidate, error);
            return error ? candidate : absolute;
        }
        error.clear();
    }

    auto absolute = std::filesystem::absolute(include, error);
    return error ? include : absolute;
}

bool decimal_digit(char c) {
    return c >= '0' && c <= '9';
}

void skip_horizontal_space(std::string_view line, std::size_t &offset) {
    while (offset < line.size()) {
        const char c = line[offset];
        if (c != ' ' && c != '\t' && c != '\v' && c != '\f' && c != '\r')
            break;
        ++offset;
    }
}

bool parse_escaped_filename(std::string_view line, std::size_t &offset,
                            std::string &filename) {
    if (offset >= line.size() || line[offset] != '"')
        return false;
    ++offset;
    filename.clear();

    while (offset < line.size()) {
        unsigned char c = static_cast<unsigned char>(line[offset++]);
        if (c == '"')
            return true;
        if (c != '\\') {
            filename.push_back(static_cast<char>(c));
            continue;
        }
        if (offset >= line.size())
            return false;

        c = static_cast<unsigned char>(line[offset++]);
        switch (c) {
        case 'a': filename.push_back('\a'); break;
        case 'b': filename.push_back('\b'); break;
        case 'f': filename.push_back('\f'); break;
        case 'n': filename.push_back('\n'); break;
        case 'r': filename.push_back('\r'); break;
        case 't': filename.push_back('\t'); break;
        case 'v': filename.push_back('\v'); break;
        case '\\':
        case '"':
        case '\'':
            filename.push_back(static_cast<char>(c));
            break;
        case 'x': {
            unsigned value = 0;
            unsigned digits = 0;
            while (offset < line.size()) {
                const unsigned char h =
                    static_cast<unsigned char>(line[offset]);
                unsigned digit;
                if (h >= '0' && h <= '9')
                    digit = h - '0';
                else if (h >= 'a' && h <= 'f')
                    digit = h - 'a' + 10;
                else if (h >= 'A' && h <= 'F')
                    digit = h - 'A' + 10;
                else
                    break;
                value = (value << 4U) | digit;
                ++offset;
                ++digits;
            }
            if (digits == 0)
                filename.push_back('x');
            else
                filename.push_back(static_cast<char>(value & 0xffU));
            break;
        }
        default:
            if (c >= '0' && c <= '7') {
                unsigned value = c - '0';
                for (unsigned digit_count = 1;
                     digit_count < 3 && offset < line.size() &&
                     line[offset] >= '0' && line[offset] <= '7';
                     ++digit_count) {
                    value = (value << 3U) |
                            static_cast<unsigned>(line[offset++] - '0');
                }
                filename.push_back(static_cast<char>(value & 0xffU));
            } else {
                // C accepts implementation-defined escapes in header names.
                // Keeping the escaped character is the least surprising map.
                filename.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return false;
}

struct LineMarker {
    unsigned line = 0;
    std::optional<std::string> file;
};

std::optional<LineMarker> parse_line_marker(std::string_view physical_line) {
    // Accept both standard `#line 12 "file"` directives and GCC-style
    // linemarkers (`# 12 "file" 1 3`). Only complete directive lines are
    // removed; a '#' in a string or ordinary source line is untouched.
    if (!physical_line.empty() && physical_line.back() == '\n')
        physical_line.remove_suffix(1);

    std::size_t offset = 0;
    skip_horizontal_space(physical_line, offset);
    if (offset >= physical_line.size() || physical_line[offset++] != '#')
        return std::nullopt;
    skip_horizontal_space(physical_line, offset);

    const bool standard = physical_line.substr(offset, 4) == "line";
    if (standard) {
        offset += 4;
        if (offset >= physical_line.size() ||
            !std::isspace(static_cast<unsigned char>(physical_line[offset])))
            return std::nullopt;
        skip_horizontal_space(physical_line, offset);
    }

    if (offset >= physical_line.size() ||
        !decimal_digit(physical_line[offset]))
        return std::nullopt;

    unsigned long long number = 0;
    while (offset < physical_line.size() &&
           decimal_digit(physical_line[offset])) {
        number = number * 10U +
                 static_cast<unsigned>(physical_line[offset++] - '0');
        if (number > std::numeric_limits<unsigned>::max())
            return std::nullopt;
    }

    LineMarker marker;
    marker.line = static_cast<unsigned>(number);
    skip_horizontal_space(physical_line, offset);
    if (offset < physical_line.size() && physical_line[offset] == '"') {
        std::string filename;
        if (!parse_escaped_filename(physical_line, offset, filename))
            return std::nullopt;
        marker.file = std::move(filename);
        skip_horizontal_space(physical_line, offset);
    }
    if (standard)
        return offset == physical_line.size() ? std::optional{std::move(marker)}
                                              : std::nullopt;
    if (!marker.file)
        return std::nullopt;

    // GCC's remaining fields are decimal flags. Reject other trailing text so
    // that a source line such as `# 1 + 2` is not mistaken for a marker.
    while (offset < physical_line.size()) {
        if (!decimal_digit(physical_line[offset]))
            return std::nullopt;
        while (offset < physical_line.size() &&
               decimal_digit(physical_line[offset]))
            ++offset;
        skip_horizontal_space(physical_line, offset);
    }
    return marker;
}

// Scan preserved lines so a marker-like line in a multi-line comment or
// continued string is never mistaken for a real preprocessing directive.
struct LexicalContext {
    bool block_comment = false;
    bool line_comment = false;
    char quote = 0;

    std::optional<std::size_t> marker_offset(std::string_view line,
                                             bool &preserve_prefix) const {
        if (line_comment || quote)
            return std::nullopt;
        std::size_t offset = 0;
        bool in_comment = block_comment;
        preserve_prefix = false;
        while (offset < line.size()) {
            if (in_comment) {
                const auto end = line.find("*/", offset);
                if (end == std::string_view::npos)
                    return std::nullopt;
                offset = end + 2;
                in_comment = false;
                preserve_prefix = true;
            }
            skip_horizontal_space(line, offset);
            if (line.substr(offset, 2) != "/*")
                break;
            offset += 2;
            in_comment = true;
        }
        if (offset < line.size() && line[offset] == '#')
            return offset;
        return std::nullopt;
    }

    void advance(std::string_view line) {
        if (!line.empty() && line.back() == '\n')
            line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const bool continued = !line.empty() && line.back() == '\\';
        if (continued)
            line.remove_suffix(1);
        for (std::size_t i = 0; i < line.size() && !line_comment; ++i) {
            const char c = line[i];
            const char next = i + 1 < line.size() ? line[i + 1] : '\0';
            if (block_comment) {
                if (c == '*' && next == '/') {
                    block_comment = false;
                    ++i;
                }
            } else if (quote) {
                if (c == '\\' && next)
                    ++i;
                else if (c == quote)
                    quote = 0;
            } else if (c == '/' && next == '*') {
                block_comment = true;
                ++i;
            } else if (c == '/' && next == '/') {
                line_comment = true;
            } else if (c == '\'' || c == '"') {
                quote = c;
            }
        }
        if (!continued) {
            quote = 0;
            line_comment = false;
        }
    }
};

Preprocessed strip_line_markers(std::string source, const std::string &file) {
    Preprocessed result;
    result.file = file;

    std::string mapped_file = file;
    unsigned mapped_line = 1;
    LexicalContext context;
    std::size_t offset = 0;
    while (offset < source.size()) {
        const std::size_t newline = source.find('\n', offset);
        const std::size_t end =
            newline == std::string::npos ? source.size() : newline + 1;
        const std::string_view physical(source.data() + offset, end - offset);

        bool preserve_prefix = false;
        const auto marker_offset = context.marker_offset(physical, preserve_prefix);
        const auto marker = marker_offset
                                ? parse_line_marker(physical.substr(*marker_offset))
                                : std::nullopt;
        if (marker) {
            if (preserve_prefix) {
                result.text.append(physical.substr(0, *marker_offset));
                if (physical.ends_with("\r\n"))
                    result.text.append("\r\n");
                else if (physical.ends_with('\n'))
                    result.text.push_back('\n');
                result.lines.push_back({mapped_file, mapped_line, 0});
                context.block_comment = false;
            }
            mapped_line = marker->line;
            if (marker->file)
                mapped_file = *marker->file;
        } else {
            context.advance(physical);
            result.text.append(physical);
            result.lines.push_back({mapped_file, mapped_line, 0});
            if (mapped_line != std::numeric_limits<unsigned>::max())
                ++mapped_line;
        }
        offset = end;
    }
    result.output = std::move(source);
    return result;
}

struct GeneratedLineMarker {
    std::size_t begin;
    std::size_t end;
    Location location;
};

void record_line_marker(void *context, std::size_t begin, std::size_t end,
                        long line, const char *file) {
    auto &markers = *static_cast<std::vector<GeneratedLineMarker> *>(context);
    markers.push_back({begin, end, {file, line_number(line), 0}});
}

Preprocessed strip_generated_markers(
    std::string source, const std::string &file,
    const std::vector<GeneratedLineMarker> &markers) {
    Preprocessed result;
    result.file = file;
    Location current{file, 1, 0};
    std::size_t offset = 0;

    auto append_until = [&](std::size_t stop) {
        while (offset < stop) {
            const std::string_view chunk(source.data() + offset, stop - offset);
            const auto newline = chunk.find('\n');
            if (newline == std::string_view::npos) {
                // A marker can follow a preserved comment. Its terminating
                // newline keeps that prefix on a separate output line.
                if (chunk.find_first_not_of(" \t\v\f\r") != std::string_view::npos) {
                    result.text.append(chunk);
                    result.text.push_back('\n');
                    result.lines.push_back(current);
                }
                break;
            }
            result.text.append(chunk.substr(0, newline + 1));
            result.lines.push_back(current);
            if (current.line != std::numeric_limits<unsigned>::max())
                ++current.line;
            offset += newline + 1;
        }
        offset = stop;
    };

    for (const auto &marker : markers) {
        append_until(marker.begin);
        current = marker.location;
        offset = marker.end;
    }
    append_until(source.size());
    result.output = std::move(source);
    return result;
}

void emit_host_error(Diagnostics &diagnostics, const char *code,
                     std::string message, const std::string &file) {
    diagnostics.error(code, std::move(message), {file, 0, 0});
}

struct ActiveDiagnosticScope {
    explicit ActiveDiagnosticScope(UcppDiagnosticContext &context)
        : previous(active_diagnostics) {
        active_diagnostics = &context;
    }
    ~ActiveDiagnosticScope() { active_diagnostics = previous; }

    UcppDiagnosticContext *previous;
};

struct DirectiveEvent {
    Location location;
    std::size_t offset;
    unsigned resume_line;
    std::string name;
    std::string arguments;
    const char *error = nullptr;
};

struct PragmaContext {
    const std::vector<std::string> &gcc_pragma_names;
    std::vector<DirectiveEvent> events{};
    std::size_t bytes = 0;
    std::exception_ptr exception{};
};

bool pragma_whitespace(const token &t) {
    return t.type == NONE || t.type == OPT_NONE || t.type == COMMENT || t.type == NEWLINE;
}

int record_text_pragma(void *opaque, token_fifo *tokens, long line, const char *file,
                       int can_emit, std::size_t offset, long resume_line) noexcept {
    auto &context = *static_cast<PragmaContext *>(opaque);
    if (context.exception)
        return 1;
    if (!context.events.empty() && context.events.back().error)
        return 0;
    std::size_t i = 0;
    while (i < tokens->nt && pragma_whitespace(tokens->t[i]))
        ++i;
    if (i == tokens->nt || tokens->t[i].type != NAME)
        return 0;
    const std::string_view space_name(tokens->t[i].name);
    if (space_name != "cephyr" && space_name != "GCC")
        return 0;
    do {
        ++i;
    } while (i < tokens->nt && pragma_whitespace(tokens->t[i]));
    const auto name = i < tokens->nt && tokens->t[i].type == NAME
        ? std::string_view(tokens->t[i].name) : std::string_view{};
    // Unclaimed GCC pragmas retain their normal behavior and do not spend
    // SDK budgets. In the reserved Cephyr namespace, missing names are errors.
    const auto &gcc_names = context.gcc_pragma_names;
    if (space_name == "GCC" &&
        (name.empty() || std::ranges::find(gcc_names, name) == gcc_names.end()))
        return 0;
    try {
        const std::string_view filename = file ? file : "";
        auto &event = context.events.emplace_back(DirectiveEvent{
            {std::string(filename.substr(0, CEPHYR_SDK_MAX_FILE_BYTES)), line_number(line), 0},
            offset, line_number(resume_line), {}, {}, nullptr
        });
        const auto fail = [&](const char *message) {
            event.error = message;
            return 0; // Diagnose in order, after ucpp has released its lock.
        };
        if (context.events.size() > CEPHYR_SDK_MAX_DIRECTIVE_INVOCATIONS)
            return fail("directive invocation limit exceeded");
        if (filename.size() > CEPHYR_SDK_MAX_FILE_BYTES)
            return fail("Cephyr directive filename exceeds the size limit");
        if (!can_emit)
            return fail("Cephyr _Pragma is not supported inside a preprocessing directive");

        if (name.empty())
            return fail("expected a directive name after '#pragma cephyr'");
        if (name.size() > CEPHYR_SDK_MAX_NAME_BYTES)
            return fail("Cephyr directive name exceeds the size limit");
        event.name = name;

        // Keep punctuation and literal spellings, with one space for any
        // nonempty run of trivia between the argument tokens.
        auto &arguments = event.arguments;
        bool space = false;
        for (++i; i < tokens->nt; ++i) {
            auto &token = tokens->t[i];
            if (pragma_whitespace(token)) {
                space = !arguments.empty();
                continue;
            }
            const std::string_view spelling(ucpp_token_name(&token));
            if (spelling.size() + space > CEPHYR_SDK_MAX_DIRECTIVE_ARGUMENT_BYTES - arguments.size())
                return fail("Cephyr directive arguments exceed the size limit");
            if (space)
                arguments.push_back(' ');
            arguments.append(spelling);
            space = false;
        }

        const auto bytes = filename.size() + name.size() + arguments.size();
        if (bytes > CEPHYR_SDK_MAX_SOURCE_BYTES - context.bytes)
            return fail("total Cephyr directive input exceeds the size limit");
        context.bytes += bytes;
        return 0;
    } catch (...) {
        // Do not unwind through ucpp's C stack; let it finish cleaning up.
        context.exception = std::current_exception();
        return 1;
    }
}

bool apply_directives(const PragmaContext &context, const cephyr::PreprocessOptions &options,
                      Diagnostics &diagnostics, std::string &source,
                      std::vector<GeneratedLineMarker> &markers) {
    std::string output;
    std::vector<GeneratedLineMarker> output_markers;
    std::size_t offset = 0, marker_index = 0;
    auto append_original = [&](std::size_t end) {
        // Events never occur inside a marker. Original markers beginning at
        // this offset belong to the next chunk, after the insertion.
        while (marker_index < markers.size() && markers[marker_index].begin < end) {
            auto marker = markers[marker_index++];
            marker.begin = output.size() + (marker.begin - offset);
            marker.end = output.size() + (marker.end - offset);
            output_markers.push_back(std::move(marker));
        }
        output.append(source, offset, end - offset);
        offset = end;
    };
    auto append_marker = [&](const Location &where) {
        const auto begin = output.size();
        output += "# " + std::to_string(where.line) + " " +
                  quote_directive_filename(where.file) + "\n";
        output_markers.push_back({begin, output.size(), where});
    };

    for (const auto &event : context.events) {
        if (event.error) {
            diagnostics.error("cephyr.sdk.directive", event.error, event.location);
            return false;
        }
        if (!options.directive) {
            diagnostics.error("cephyr.sdk.directive",
                              "unknown Cephyr directive '" + event.name + "'", event.location);
            return false;
        }
        std::optional<std::string> replacement;
        const auto errors_before = diagnostics.error_count();
        if (!options.directive(event.name, event.arguments, event.location, replacement)) {
            if (diagnostics.error_count() == errors_before)
                diagnostics.error("cephyr.sdk.directive", "directive callback failed without a diagnostic",
                                  event.location);
            return false;
        }
        if (!replacement || replacement->empty())
            continue;
        // Bracket the emitted fragment with markers. The fresh line on each
        // side also prevents token merging for inline _Pragma expansions.
        append_original(event.offset);
        if (!output.empty() && output.back() != '\n')
            output.push_back('\n');
        append_marker(event.location);
        output.append(*replacement);
        if (output.back() != '\n')
            output.push_back('\n');
        append_marker({event.location.file, event.resume_line, 0});
    }
    if (!output.empty()) {
        append_original(source.size());
        source.swap(output);
        markers.swap(output_markers);
    }
    return true;
}

struct MemoryOutput {
    char *bytes = nullptr;
    std::size_t size = 0;
    FILE *stream = open_memstream(&bytes, &size);

    int close() {
        auto *file = stream;
        stream = nullptr;
        return std::fclose(file);
    }

    ~MemoryOutput() {
        if (stream)
            close();
        std::free(bytes);
    }
};

struct LexerScope {
    lexer_state *lexer;

    void finish() {
        free_lexer_state(lexer);
        wipeout();
        lexer = nullptr;
    }

    ~LexerScope() {
        if (lexer)
            finish();
    }
};

const char *literal_error(std::string_view body, std::uint32_t escape_max) {
    for (std::size_t offset = 0; offset < body.size();) {
        if (body[offset++] != '\\')
            continue;
        if (offset == body.size())
            return "unterminated escape sequence";

        const char escape = body[offset++];
        if (std::string_view("'\"?\\abfnrtv").find(escape) != std::string_view::npos)
            continue;
        if (escape == 'u' || escape == 'U') {
            std::uint32_t code_point = 0;
            const unsigned digits = escape == 'u' ? 4 : 8;
            for (unsigned i = 0; i < digits; ++i) {
                const int digit = offset < body.size() ? c11_hex_value(body[offset++]) : -1;
                if (digit < 0)
                    return "malformed universal character name in literal";
                code_point = code_point * 16 + static_cast<unsigned>(digit);
            }
            if (!c11_valid_ucn(code_point))
                return "invalid code point in universal character name in literal";
            continue;
        }
        if (escape == 'x') {
            const auto start = offset;
            std::uint32_t value = 0;
            while (offset < body.size()) {
                const int hex = c11_hex_value(body[offset]);
                if (hex < 0)
                    break;
                const auto digit = static_cast<unsigned>(hex);
                if (value > (escape_max - digit) / 16)
                    return "numeric escape out of range for literal prefix";
                value = value * 16 + digit;
                ++offset;
            }
            if (offset == start)
                return "hexadecimal escape requires a digit";
        } else if (escape >= '0' && escape <= '7') {
            unsigned value = static_cast<unsigned>(escape - '0');
            for (unsigned i = 1; i < 3 && offset < body.size() &&
                                 body[offset] >= '0' && body[offset] <= '7'; ++i)
                value = value * 8 + static_cast<unsigned>(body[offset++] - '0');
            if (value > escape_max)
                return "numeric escape out of range for literal prefix";
        } else {
            return "invalid escape sequence in literal";
        }
    }
    return nullptr;
}

// Check C11 lexical constraints on the surviving, expanded tokens without
// changing their spellings (-E), execution values, or token boundaries.
// This must run under ucpp_mutex with the raw lexer tables initialized.
bool check_literals(Preprocessed &result, Diagnostics &diagnostics) {
    UcppDiagnosticContext context{&diagnostics, result.file, 0, &result};
    ActiveDiagnosticScope scope(context);
    struct lexer_state lexer {};
    init_lexer_state(&lexer);
    lexer.flags = LEXER | PHASE3_INPUT | CPLUSPLUS_COMMENTS | UTF8_SOURCE;
    lexer.input_string = reinterpret_cast<unsigned char *>(result.text.data());
    lexer.ebuf = result.text.size();

    std::string_view previous_prefix;
    int status;
    while ((status = ucpp_next_token(&lexer)) == 0 && context.errors == 0) {
        const auto &token = *lexer.ctok;
        if (token.type == NONE || token.type == COMMENT ||
            token.type == NEWLINE || token.type == OPT_NONE)
            continue;
        if (token.type != STRING && token.type != CHAR) {
            previous_prefix = {};
            continue;
        }

        const bool string = token.type == STRING;
        const std::string_view spelling(token.name);
        const char delimiter = string ? '"' : '\'';
        const auto quote = spelling.find(delimiter);
        if (quote == std::string_view::npos || quote + 1 >= spelling.size() ||
            spelling.back() != delimiter) {
            diagnostics.error("cephyr.preprocessor.literal", "malformed literal spelling",
                              result.locate(line_number(token.line)));
            ++context.errors;
            break;
        }
        const auto prefix = spelling.substr(0, quote);
        const auto body = spelling.substr(quote + 1, spelling.size() - quote - 2);
        const bool wide = !prefix.empty() && prefix != "u8";
        // All built-in targets have 8-bit char, 16-bit char16_t, and
        // 32-bit wchar_t/char32_t (use the unsigned range for wchar_t).
        const std::uint32_t escape_max = prefix == "u" ? 0xffffu :
            wide ? 0xffffffffu : 0xffu;
        const char *message = literal_error(body, escape_max);
        if (!message && !string && body.empty())
            message = "empty character constant";
        if (!message && string && !prefix.empty() &&
            !previous_prefix.empty() && prefix != previous_prefix)
            message = wide && previous_prefix != "u8" ?
                "adjacent string literals cannot mix different wide prefixes" :
                "adjacent string literals cannot mix a UTF-8 prefix with a wide prefix";
        if (message) {
            diagnostics.error("cephyr.preprocessor.literal", message,
                              result.locate(line_number(token.line)));
            ++context.errors;
        }
        if (!string || !prefix.empty())
            // The raw lexer may reuse token.name on the next iteration.
            previous_prefix = !string ? "" : !wide ? "u8" :
                              prefix == "L" ? "L" : prefix == "u" ? "u" : "U";
    }
    free_lexer_state(&lexer);
    return status == 2 && context.errors == 0; // Raw lexer EOF, not CPPERR_EOF.
}

} // namespace

extern "C" void ucpp_error(long line, char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    report_ucpp(false, "cephyr.preprocessor.error", line, format, arguments);
    va_end(arguments);
}

extern "C" void ucpp_warning(long line, char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    report_ucpp(true, "cephyr.preprocessor.warning", line, format, arguments);
    va_end(arguments);
}

extern "C" void ucpp_ouch(char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    report_ucpp(false, "cephyr.preprocessor.internal", 0, format, arguments);
    va_end(arguments);
}

namespace cephyr {

bool valid_directive_output(std::string_view text) {
    if (text.empty())
        return true;
    // PHASE3_INPUT disables translation phases 1/2 (including UTF-8
    // validation, which is performed here). Reject carriage returns so the
    // literal scanner, parser and emitted line map agree on line boundaries.
    c11_utf8_state utf8{};
    for (const unsigned char c : text) {
        if ((c < ' ' && c != '\n' && c != '\t' && c != '\v' && c != '\f') || c == 127 ||
            c11_utf8_feed(&utf8, c) < 0)
            return false;
    }
    if (utf8.remaining)
        return false;

    // Use a separate raw lexer, not CPP. Suppress its low-level diagnostics;
    // a rejected sink submission produces a callback diagnostic.
    std::lock_guard lock(ucpp_mutex());
    UcppDiagnosticContext silent;
    ActiveDiagnosticScope scope(silent);
    struct lexer_state lexer {};
    init_lexer_state(&lexer);
    lexer.flags = LEXER | PHASE3_INPUT | CPLUSPLUS_COMMENTS | UTF8_SOURCE;
    lexer.input_string = reinterpret_cast<unsigned char *>(const_cast<char *>(text.data()));
    lexer.ebuf = text.size();
    int status;
    while ((status = ucpp_next_token(&lexer)) == 0 && !lexer.character_error) {
        const auto &token = *lexer.ctok;
        if (token.type == COMMENT || token.type == BUNCH || token.type == SHARP ||
            token.type == DSHARP || token.type == DIG_SHARP || token.type == DIG_DSHARP ||
            (token.type == NAME && std::strcmp(token.name, "_Pragma") == 0))
            break;
    }
    const bool valid = status == 2 && !lexer.character_error;
    free_lexer_state(&lexer);
    return valid;
}

Location Preprocessed::locate(unsigned line, unsigned column) const {
    if (line == 0)
        return {file, 0, column};
    if (!line_runs.empty()) {
        const auto next = std::ranges::upper_bound(line_runs, line, {}, &decltype(line_runs)::value_type::first);
        if (next != line_runs.begin()) {
            const auto &[physical, loc] = *std::prev(next);
            const auto mapped = std::min<std::uint64_t>(std::uint64_t{loc.line} + line - physical,
                                                        std::numeric_limits<unsigned>::max());
            return {loc.file, static_cast<unsigned>(mapped), column};
        }
    }
    if (line <= lines.size()) {
        Location location = lines[line - 1];
        location.column = column;
        return location;
    }
    if (lines.empty())
        return {file, line, column};

    Location location = lines.back();
    const unsigned delta = line - static_cast<unsigned>(lines.size());
    if (location.line <= std::numeric_limits<unsigned>::max() - delta)
        location.line += delta;
    else
        location.line = std::numeric_limits<unsigned>::max();
    location.column = column;
    return location;
}

std::optional<Preprocessed> preprocess(const std::string &source,
                                       const std::string &file,
                                       const PreprocessOptions &options,
                                       Diagnostics &diagnostics) {
    const auto errors_before = diagnostics.error_count();
    std::unique_lock lock(ucpp_mutex());
    UcppDiagnosticContext diagnostic_context{&diagnostics, file, 0};
    ActiveDiagnosticScope diagnostic_scope(diagnostic_context);
    PragmaContext pragma_context{options.gcc_pragma_names};

    MemoryOutput capture;
    auto *output = capture.stream;
    if (!output) {
        emit_host_error(diagnostics, "cephyr.preprocessor.io",
                        "unable to create preprocessor output stream: " +
                            std::string(std::strerror(errno)),
                        file);
        return std::nullopt;
    }

    init_cpp();
    no_special_macros = 0;
    c99_compliant = 1;
    c99_hosted = options.hosted ? 1 : 0;
    ucpp_c11_atomics = options.native_atomics;
    emit_dependencies = 0;
    emit_defines = 0;
    emit_assertions = 0;
    emit_output = output;
    transient_characters = nullptr;
    init_tables(1);
    init_include_path(nullptr);

    std::vector<GeneratedLineMarker> markers;
    std::string synthetic_source;
    struct lexer_state lexer {};
    init_lexer_state(&lexer);
    LexerScope lexer_scope{&lexer};
    lexer.line_marker = record_line_marker;
    lexer.line_marker_context = &markers;
    lexer.flags = DEFAULT_CPP_FLAGS | LINE_NUM | GCC_LINE_NUM | COPY_LINE |
                  CHECK_SOURCE_EOF | PROCESS_TEXT_PRAGMAS;
    if (!options.trigraphs)
        lexer.flags &= ~HANDLE_TRIGRAPHS;
    if (options.keep_comments)
        lexer.flags &= ~DISCARD_COMMENTS;
    if (options.assembler)
        lexer.flags |= ASSEMBLY_INPUT;
    else {
        lexer.text_pragma = record_text_pragma;
        lexer.text_pragma_context = &pragma_context;
        lexer.flags |= UTF8_SOURCE | C11_CONSTRAINTS;
        if (options.plain_char_signed)
            lexer.flags |= PP_CHAR_SIGNED;
        if (!options.wchar_signed)
            lexer.flags |= PP_WCHAR_UNSIGNED;
    }
    lexer.output = output;

    if (!options.forced_includes.empty()) {
        for (const auto &include : options.forced_includes) {
            synthetic_source += "#include ";
            synthetic_source += quote_directive_filename(
                resolve_forced_include(include, options).string());
            synthetic_source.push_back('\n');
        }
        // Each forced include returns to the original file context; reset
        // only the line number so the synthetic prefix is never user-visible.
        synthetic_source += "#line 1\n";
    }
    synthetic_source += source;

    lexer.input = nullptr;
    lexer.input_string =
        reinterpret_cast<unsigned char *>(synthetic_source.data());
    lexer.pbuf = 0;
    lexer.ebuf = synthetic_source.size();

    std::string mutable_file = file;
    set_init_filename(mutable_file.data(), 0);

    for (const auto &directory : options.include_dirs) {
        std::string path = directory.string();
        add_incpath(path.data());
    }
    for (const auto &directory : options.system_include_dirs) {
        std::string path = directory.string();
        add_incpath(path.data());
    }

    bool failed = false;

    for (const auto &[define, macro] : options.macros) {
        std::string mutable_macro = macro;
        const int macro_failed =
            define ? define_macro(&lexer, mutable_macro.data())
                   : undef_macro(&lexer, mutable_macro.data());
        failed = macro_failed || failed;
    }

    enter_file(&lexer, lexer.flags);
    int status;
    while ((status = cpp(&lexer)) < CPPERR_EOF)
        failed = (status > 0) || failed;
    failed = check_cpp_errors(&lexer) || failed;

    const bool write_failed = std::ferror(output);
    lexer_scope.finish();
    const int close_status = capture.close();
    lock.unlock();
    if (pragma_context.exception)
        std::rethrow_exception(pragma_context.exception);
    if (write_failed || close_status != 0 || !capture.bytes) {
        emit_host_error(diagnostics, "cephyr.preprocessor.io",
                        "unable to read preprocessor output", file);
        return std::nullopt;
    }

    if (failed && diagnostic_context.errors == 0)
        emit_host_error(diagnostics, "cephyr.preprocessor.error",
                        "preprocessing failed without a diagnostic", file);
    if (failed || diagnostic_context.errors != 0 ||
        (!pragma_context.events.empty() && diagnostics.error_count() != errors_before))
        return std::nullopt;

    std::string preprocessed(capture.bytes, capture.size);
    if (!apply_directives(pragma_context, options, diagnostics, preprocessed, markers))
        return std::nullopt;
    auto result = strip_generated_markers(std::move(preprocessed), file, markers);
    if (!options.assembler && result.text.find_first_of("\"'") != std::string::npos) {
        std::lock_guard literal_lock(ucpp_mutex());
        if (!check_literals(result, diagnostics))
            return std::nullopt;
    }
    if (!options.line_markers)
        result.output = result.text;
    return result;
}

std::optional<Preprocessed> read_preprocessed(const std::string &source,
                                              const std::string &file,
                                              Diagnostics &diagnostics) {
    auto result = strip_line_markers(source, file);
    std::lock_guard lock(ucpp_mutex());
    ucpp_init_cppm();
    if (!check_literals(result, diagnostics))
        return std::nullopt;
    return result;
}

} // namespace cephyr
