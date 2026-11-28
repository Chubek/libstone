#include <smtlib2/builder.hpp>
#include <smtlib2/protocol.hpp>
#include <iostream>
#include <sstream>

namespace {
using namespace smtlib2;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class Action>
void rejects(Action action, const char* message) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void builder() {
    ScriptBuilder script, other;
    const auto x = script.declare_bool("x");
    const auto y = script.declare_bool("|a name; (with parens)|");
    const auto outsider = other.declare_bool("x");
    const auto truth = BoolTerm::literal(true);
    const auto falsity = BoolTerm::literal(false);
    const auto old = script.text();
    rejects<Error>([&] { script.declare_bool("|x|"); }, "expected canonical duplicate");
    rejects<Error>([&] { script.declare_bool("|not|"); }, "expected reserved rejection");
    rejects<Error>([&] { script.declare_bool("k) (exit)"); }, "expected invalid symbol rejection");
    rejects<std::invalid_argument>([&] { script.assert_term(outsider); }, "expected foreign declaration rejection");
    rejects<std::invalid_argument>([&] { BoolTerm::equal(x, outsider); }, "expected mixed declarations rejection");
    require(script.text() == old, "rejected builder calls mutated script");
    script.assert_term(BoolTerm::conjunction(x, truth));
    script.assert_term(BoolTerm::ite(x, y, falsity));
    script.assert_term(BoolTerm::implies(y, x));
    script.check_sat();
    script.assert_term(BoolTerm::disjunction(BoolTerm::negate(y), BoolTerm::exclusive_or(x, x)));
    script.check_sat();
    script.exit();
    rejects<std::logic_error>([&] { script.check_sat(); }, "append after exit should fail");
    Session session;
    const auto responses = session.execute(script.text());
    require(responses.size() == 2 && responses[0].text == "sat" && responses[1].text == "unsat",
            "built script should check sat then unsat");
}

void protocol() {
    Session session;
    std::istringstream input("; out of band )\r\n"
                             "(declare-const |) ; x| Bool)"
                             "(assert ; )\r\n |) ; x|)"
                             "(echo \"(hello; \"\"quoted\"\"\")"
                             "(check-sat)(exit)trailing bytes");
    std::ostringstream output;
    run_protocol(session, input, output, Limits{40, 20, 4});
    require(session.exited() && input.peek() == 't', "protocol read beyond exit");
    require(output.str() == "\"(hello; \"\"quoted\"\"\"\nsat\n", "unexpected streamed responses");

    Session broken;
    std::istringstream bad_input("(check-sat)\r\n  (assert\n");
    std::ostringstream bad_output;
    try {
        run_protocol(broken, bad_input, bad_output);
        throw std::runtime_error("protocol accepted an unterminated command");
    } catch (const Error& error) {
        require(bad_output.str() == "sat\n", "completed command response lost");
        const auto begin = error.span().begin;
        require(begin.line == 2 && begin.column == 3 && begin.offset == 15,
                "protocol span did not include stream prefix");
    }

    Session bounded;
    std::istringstream large("(check-sat       )"), deep("(assert (not true))"), empty(" ; )\n\t");
    std::ostringstream unused;
    rejects<Error>([&] { run_protocol(bounded, large, unused, Limits{12, 20, 4}); },
                   "expected per-command byte limit");
    rejects<Error>([&] { run_protocol(bounded, deep, unused, Limits{32, 20, 1}); },
                   "expected per-command depth limit");
    run_protocol(bounded, empty, unused);
    require(unused.str().empty(), "blank input emitted response");
}
}

int main() {
    try {
        builder();
        protocol();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
