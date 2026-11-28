#include <smtlib2/protocol.hpp>
#include <iostream>
#include <sstream>

namespace {
using namespace smtlib2;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct CountingOutput : std::stringbuf {
    unsigned flushes = 0;
    int sync() override { ++flushes; return 0; }
};

bool is_wire_error(const SExpr& response) {
    return response.is_list && response.children.size() == 2
        && response.children[0].atom == "error"
        && response.children[1].kind == TokenKind::string_literal;
}

void default_errors_keep_stream_spans() {
    Session session;
    const std::string prefix = "; ignored )\r\n(check-sat)\r\n  ";
    const std::string malformed = "(assert #xG)";
    std::istringstream input(prefix + malformed + "(exit)");
    std::ostringstream output;
    try {
        run_protocol(session, input, output);
    } catch (const Error& error) {
        require(error.category() == ErrorCategory::lexical, "default lost error category");
        const auto span = error.span();
        const auto offset = prefix.size() + malformed.find('#');
        require(span.begin.offset == offset && span.end.offset == offset + 3
                    && span.begin.line == 3 && span.end.line == 3
                    && span.begin.column == 11 && span.end.column == 14,
                "default did not translate both endpoints to stream positions");
        require(output.str() == "sat\n" && input.peek() == '(' && !session.exited(),
                "default rendered an error or consumed a later command");
        return;
    }
    throw std::runtime_error("default failed to throw on a malformed token");
}

void report_and_continue() {
    Session session;
    std::istringstream input("(get-info :error-behavior)"
                             "(declare-const |a \" ) (exit) ;| Bool)"
                             "(declare-const |a \" ) (exit) ;| Bool)"
                             "(assert false extra)"
                             "(assert #xG)"
                             "(assert |broken\nsymbol|)"
                             "(echo \"text with ) and \"\"quotes\"\"\")"
                             "(assert |a \" ) (exit) ;|)"
                             "(check-sat)(exit)unread");
    CountingOutput buffer;
    std::ostream output(&buffer);
    run_protocol(session, input, output, {}, ProtocolErrorPolicy::report_and_continue);
    require(session.exited() && input.peek() == 'u', "protocol read past exit");
    const auto replies = parse(buffer.str());
    require(replies.size() == 7 && buffer.flushes == 7, "wrong response or flush count");
    require(print(replies[0]) == "(:error-behavior continued-execution)", "wrong wire policy query");
    require(is_wire_error(replies[1]) && is_wire_error(replies[2]) && is_wire_error(replies[3])
                && is_wire_error(replies[4]),
            "missing errors for framed semantic, syntax, and lexical failures");
    require(replies[1].children[1].atom.find("|a \"\" ) (exit) ;|") != std::string::npos,
            "diagnostic did not double an embedded quote");
    require(replies[4].children[1].atom.find('\n') == std::string::npos,
            "diagnostic caused an extra wire line");
    require(replies[5].atom == "\"text with ) and \"\"quotes\"\"\"" && replies[6].atom == "sat",
            "failed commands changed state or corrupted the next response");
}

void terminal_wire_errors() {
    struct Test {
        const char* source;
        Limits limits;
        ProtocolErrorPolicy policy;
        const char* diagnostic;
        int next;
    };
    constexpr auto recover = ProtocolErrorPolicy::report_and_continue;
    const Test tests[] = {
        {"(assert false extra)(exit)", {}, ProtocolErrorPolicy::report_and_stop, "", '('},
        {"!(exit)", {}, recover, "expected command list", '('},
        {"(assert x\"bad\")(exit)", {}, recover, "invalid character in atom", 'b'},
        {"(assert |bad\\name|)(exit)", {}, recover, "backslash in quoted symbol", 'n'},
        {"(echo \"unterminated (exit)", {}, recover, "unterminated string", std::char_traits<char>::eof()},
        {"(check-sat   )(exit)", Limits{11, 20, 4}, recover, "input size limit", ' '},
        {"(assert (not true))(exit)", Limits{32, 20, 1}, recover, "nesting limit", 'n'},
        {"(assert true)(exit)", Limits{32, 3, 4}, recover, "token limit", '('},
        {"(echo \"do not read me\")(exit)", Limits{64, 2, 4}, recover, "token limit", 'd'},
        {"(check-sat)(exit)", Limits{64, 0, 4}, recover, "token limit", 'c'},
        {"(push 1025)(exit)", {}, recover, "scope limit", '('},
    };
    for (const auto& test : tests) {
        Session session;
        std::istringstream input(test.source);
        CountingOutput buffer;
        std::ostream output(&buffer);
        run_protocol(session, input, output, test.limits, test.policy);
        require(!session.exited() && input.peek() == test.next, "terminal error consumed more input");
        const auto replies = parse(buffer.str());
        require(replies.size() == 1 && buffer.flushes == 1 && is_wire_error(replies[0]),
                "terminal error did not emit and flush exactly one wire error");
        require(replies[0].children[1].atom.find(test.diagnostic) != std::string::npos,
                "unexpected terminal diagnostic");
    }
}

void effective_depth_stops_unfinished_frame() {
    Session session;
    const std::string nested(Limits{}.max_depth + 1, '(');
    std::istringstream input("(check-sat)\r\n" + nested + "unread");
    std::ostringstream output;
    Limits limits;
    limits.max_depth = Limits{}.max_depth + 20;
    try {
        run_protocol(session, input, output, limits);
    } catch (const Error& error) {
        require(error.category() == ErrorCategory::resource_limit
                    && error.span().begin.line == 2
                    && error.span().begin.column == nested.size()
                    && error.span().end.column == nested.size() + 1,
                "framer did not apply the session nesting bound");
        require(input.peek() == 'u' && output.str() == "sat\n",
                "framer consumed bytes past excess nesting");
        return;
    }
    throw std::runtime_error("expected framing depth error");
}

void token_boundaries_and_exception_masked_eof() {
    Session session;
    std::istringstream input("(echo ; in-frame comment )\r\n\"a\"\"b\")"
                             "(declare-const |x () ; \" y| Bool)"
                             "(check-sat); trailing comment");
    input.exceptions(std::ios::eofbit | std::ios::failbit);
    std::ostringstream output;
    run_protocol(session, input, output, Limits{64, 5, 1});
    require(input.eof() && !input.bad() && output.str() == "\"a\"\"b\"\nsat\n",
            "legal token boundaries or ordinary EOF were misclassified");

    std::istringstream incomplete("(echo \"unterminated");
    incomplete.exceptions(std::ios::eofbit | std::ios::failbit);
    std::ostringstream errors;
    run_protocol(session, incomplete, errors, {}, ProtocolErrorPolicy::report_and_continue);
    const auto replies = parse(errors.str());
    require(replies.size() == 1 && is_wire_error(replies[0]),
            "masked EOF bypassed the unfinished command diagnostic");
}

void per_command_limits_and_solver_unknown() {
    Session session(0);
    std::istringstream input("; discarded comment much longer than byte limit )\r\n"
                             "(declare-const x Bool)(check-sat)(exit)(check-sat)");
    std::ostringstream output;
    run_protocol(session, input, output, Limits{22, 5, 1}, ProtocolErrorPolicy::report_and_continue);
    require(session.exited() && input.peek() == '(' && output.str() == "unknown\n",
            "wire mode confused an exhausted solver with an input error");
    std::istringstream later("(check-sat)");
    run_protocol(session, later, output, {}, ProtocolErrorPolicy::report_and_continue);
    require(later.peek() == '(' && output.str() == "unknown\n", "exited session read more input");
}

void one_command_exit_is_a_clean_boundary() {
    Session session;
    std::istringstream input("(exit extra)(exit)not-a-command");
    std::ostringstream output;
    constexpr auto recover = ProtocolErrorPolicy::report_and_continue;
    require(detail::drive_protocol(session, input, output, {}, recover, true) && !session.exited(),
            "malformed exit did not stop at a recoverable command boundary");
    require(detail::drive_protocol(session, input, output, {}, recover, true) && session.exited(),
            "successful exit was misclassified as a terminal error");
    require(!detail::drive_protocol(session, input, output, {}, recover, true) && input.peek() == 'n',
            "one-command driver read past exit");
    const auto replies = parse(output.str());
    require(replies.size() == 1 && is_wire_error(replies[0]), "wrong one-command replies");
}

void transport_errors_are_not_wire_errors() {
    constexpr auto recover = ProtocolErrorPolicy::report_and_continue;
    Session session;
    std::istringstream input("(assert false)(exit)");
    std::ostringstream output;
    output.setstate(std::ios::badbit);
    try {
        run_protocol(session, input, output, {}, recover);
        throw std::runtime_error("expected output failure");
    } catch (const std::ios_base::failure&) {
        require(input.tellg() == std::streampos(0) && !session.exited(),
                "driver consumed input with an already failed output stream");
        require(session.execute("(check-sat)").back().text == "sat",
                "driver dispatched with an already failed output stream");
    }
    struct FailingFlush : std::stringbuf { int sync() override { return -1; } } buffer;
    std::ostream broken_output(&buffer);
    std::istringstream late_failure("(set-option :print-success true)(assert false)");
    try {
        run_protocol(session, late_failure, broken_output, {}, recover);
        throw std::runtime_error("expected flush failure");
    } catch (const std::ios_base::failure&) {
        require(late_failure.peek() == '(' && buffer.str() == "success\n",
                "driver continued or emitted a wire error after stream flush failure");
        require(std::get<OptionResponse>(session.execute("(get-option :print-success)").back().payload).value,
                "late transport failure rolled back an already executed command");
    }
    std::istringstream bad_input;
    std::ostringstream good_output;
    bad_input.setstate(std::ios::badbit);
    try {
        run_protocol(session, bad_input, good_output, {}, recover);
        throw std::runtime_error("expected input failure");
    } catch (const std::ios_base::failure&) {
        require(good_output.str().empty(), "driver rendered an I/O failure as a command error");
    }
    FailingFlush tied_buffer;
    std::ostream tied_output(&tied_buffer);
    std::istringstream tied_input("(assert false)");
    tied_input.tie(&tied_output);
    Session tied_session;
    try {
        run_protocol(tied_session, tied_input, tied_output, {}, recover);
        throw std::runtime_error("expected tied stream flush failure");
    } catch (const std::ios_base::failure&) {
        require(tied_session.execute("(check-sat)").back().text == "sat",
                "driver dispatched after tied output failed");
    }
}
}

int main() {
    try {
        default_errors_keep_stream_spans();
        report_and_continue();
        terminal_wire_errors();
        effective_depth_stops_unfinished_frame();
        token_boundaries_and_exception_masked_eof();
        per_command_limits_and_solver_unknown();
        one_command_exit_is_a_clean_boundary();
        transport_errors_are_not_wire_errors();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
