#pragma once
#include "session.hpp"
#include <istream>
#include <ostream>
#include <string_view>

namespace smtlib2 {
enum class ProtocolErrorPolicy {
    throw_error,
    report_and_stop,
    report_and_continue
};

namespace detail {
inline std::string protocol_error_text(std::string_view message) {
    std::string text = "(error \"";
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : message) {
        // SMT-LIB uses doubled quotes, not backslash escapes. Display
        // nonprintable bytes visibly so diagnostics stay on one wire line.
        if (c == '"') text += "\"\"";
        else if (c >= ' ' && c <= '~') text += static_cast<char>(c);
        else {
            text += "\\x";
            text += hex[c >> 4];
            text += hex[c & 15];
        }
    }
    return text + "\")";
}

// In one-command mode return true on a completed command (including exit) or
// a recoverable framed error. The host checks exited() before reading again.
inline bool drive_protocol(Session& session, std::istream& input, std::ostream& output,
                           Limits limits, ProtocolErrorPolicy policy, bool one_command) {
    if (session.exited()) return false;
    if (!output) throw std::ios_base::failure("SMT-LIB protocol output failed");
    limits = session_input_limits(limits);
    enum class Mode { normal, atom, comment, quoted, string, after_quote };
    Mode mode = Mode::normal;
    Position position, start;
    bool previous_cr = false;
    std::size_t depth = 0;
    std::size_t tokens = 0;
    std::string command;
    auto absolute = [&](Position p) {
        p.offset += start.offset;
        if (p.line == 1) p.column += start.column - 1;
        p.line += start.line - 1;
        return p;
    };
    auto flush = [&] {
        output.flush();
        if (!output) throw std::ios_base::failure("SMT-LIB protocol output failed");
    };
    auto handle_error = [&](const Error& error, bool framed = false) {
        if (policy == ProtocolErrorPolicy::throw_error) throw error;
        output << detail::protocol_error_text(error.what()) << '\n';
        flush();
        return policy == ProtocolErrorPolicy::report_and_continue && framed
            && error.category() != ErrorCategory::resource_limit;
    };
    auto execute = [&](bool framed) {
        std::vector<Response> responses;
        try {
            responses = session.execute(command, limits);
        } catch (const Error& error) {
            command.clear();
            return handle_error(Error({absolute(error.span().begin), absolute(error.span().end)},
                                      error.what(), error.category()), framed);
        }
        for (const auto& response : responses) {
            const auto* info = std::get_if<InfoResponse>(&response.payload);
            if (policy == ProtocolErrorPolicy::report_and_continue
                && info && info->keyword == ":error-behavior")
                output << "(:error-behavior continued-execution)\n";
            else output << response.text << '\n';
        }
        if (!responses.empty()) flush();
        else if (!output) throw std::ios_base::failure("SMT-LIB protocol output failed");
        command.clear();
        return true;
    };
    while (!session.exited()) {
        auto next = std::char_traits<char>::eof();
        try {
            next = input.get();
        } catch (const std::ios_base::failure&) {
            // get() can throw on ordinary EOF when eofbit/failbit are masked.
            // An underlying stream failure is not clean EOF.
            if (input.bad() || !input.eof()) throw;
        }
        // The input sentry can flush the protocol's output if tied to it.
        if (!output) throw std::ios_base::failure("SMT-LIB protocol output failed");
        if (next == std::char_traits<char>::eof()) {
            if (input.bad() || !input.eof())
                throw std::ios_base::failure("SMT-LIB protocol input failed");
            if (!command.empty()) execute(false); // Parse the incomplete command for its diagnostic.
            return false;
        }
        const char c = std::char_traits<char>::to_char_type(next);
        const Position begin = position;
        ++position.offset;
        if (c == '\r' || c == '\n') {
            if (c != '\n' || !previous_cr) ++position.line;
            position.column = 1;
        } else ++position.column;
        previous_cr = c == '\r';
        if (command.empty()) {
            if (mode == Mode::comment) {
                if (c == '\r' || c == '\n') mode = Mode::normal;
                continue;
            }
            if (detail::lex_space(c)) continue;
            if (c == ';') { mode = Mode::comment; continue; }
            if (c != '(') {
                handle_error(Error({begin, position}, "expected command list", ErrorCategory::syntax));
                return false;
            }
            start = begin;
            tokens = 0;
        }
        if (command.size() >= limits.max_input_bytes) {
            handle_error(Error({start, position}, "input size limit exceeded", ErrorCategory::resource_limit));
            return false;
        }
        command += c;
        if (mode == Mode::comment) {
            if (c == '\r' || c == '\n') mode = Mode::normal;
            continue;
        }
        if (mode == Mode::quoted) {
            if (c == '|') mode = Mode::normal;
            else if (c == '\\' || !detail::lex_text_char(c)) {
                handle_error(Error({begin, position}, c == '\\' ? "backslash in quoted symbol"
                                                                 : "invalid character in quoted symbol",
                                   ErrorCategory::lexical));
                return false;
            }
            continue;
        }
        if (mode == Mode::after_quote) {
            if (c == '"') { mode = Mode::string; continue; } // Doubled quote.
            mode = Mode::normal;
        }
        if (mode == Mode::string) {
            if (c == '"') mode = Mode::after_quote;
            else if (!detail::lex_text_char(c)) {
                handle_error(Error({begin, position}, "invalid character in string literal", ErrorCategory::lexical));
                return false;
            }
            continue;
        }
        if (mode == Mode::atom && (c == '"' || c == '|')) {
            handle_error(Error({begin, position}, "invalid character in atom", ErrorCategory::lexical));
            return false;
        }
        // Match the lexer's token boundaries without buffering the rest of a
        // frame once the token cap has been reached. Delimited text is counted
        // at its opening character; doubled quotes never start another token.
        if (!detail::lex_space(c) && c != ';'
            && (mode != Mode::atom || c == '(' || c == ')')) {
            if (tokens >= limits.max_tokens) {
                handle_error(Error({begin, begin}, "token limit exceeded", ErrorCategory::resource_limit));
                return false;
            }
            ++tokens;
        }
        if (c == ';') mode = Mode::comment;
        else if (c == '|') mode = Mode::quoted;
        else if (c == '"') mode = Mode::string;
        else if (c == '(') {
            mode = Mode::normal;
            if (depth >= limits.max_depth) {
                handle_error(Error({begin, position}, "nesting limit exceeded", ErrorCategory::resource_limit));
                return false;
            }
            ++depth;
        } else if (c == ')') {
            mode = Mode::normal;
            if (--depth == 0) {
                if (!execute(true)) return false;
                if (one_command) return true;
            }
        } else if (detail::lex_space(c)) mode = Mode::normal;
        else mode = Mode::atom;
    }
    return false;
}
}

/// @implstring Drive a Session from a stream, without buffering the whole input.
/// Limits apply per command; inter-command whitespace/comments are discarded.
/// Byte, token and effective nesting limits are enforced while framing.
/// Each completed command is executed and its responses flushed before reading
/// the next one. Stop on exit or clean EOF. By default Errors propagate with
/// stream-wide spans. Wire policies instead emit and flush an SMT-LIB error.
/// report_and_stop returns on any Error; report_and_continue resumes only after
/// a fully framed command fails with a non-resource Error. Framing errors,
/// incomplete EOF, and resource limits always stop; no resynchronization scan
/// is attempted, and a wire-mode stop does not mark the Session exited.
/// The protocol's :error-behavior query reflects the selected policy.
/// I/O failures and other exceptions propagate in all modes. An already failed
/// output stream prevents dispatch; a later write failure does not roll back a
/// command whose dispatch completed.
inline void run_protocol(Session& session, std::istream& input, std::ostream& output,
                         Limits limits, ProtocolErrorPolicy policy) {
    detail::drive_protocol(session, input, output, limits, policy, false);
}

inline void run_protocol(Session& session, std::istream& input, std::ostream& output,
                         Limits limits = {}) {
    run_protocol(session, input, output, limits, ProtocolErrorPolicy::throw_error);
}
}
