#ifdef NDEBUG
#undef NDEBUG
#endif
#include <smtlib2/session.hpp>
#include <cassert>

using namespace smtlib2;

int main() {
    Session s;
    s.execute("(declare-const a Bool)(declare-const b Bool)(assert (or a b))(check-sat)");
    assert(s.encoded_assertion_count() == 1);
    s.execute("(push 2)(assert (not a))");
    assert(s.encoded_assertion_count() == 1);
    assert(s.execute("(check-sat-assuming ((not b)))").back().text == "unsat");
    assert(s.execute("(check-sat)(get-value (a b))").back().text == "((a false) (b true))");
    assert(s.encoded_assertion_count() == 2);
    for (auto bad : {"(pop 3)", "(push 1023)", "(assert missing)", "(declare-const a Bool)",
                     "(check-sat-assuming (missing))"}) {
        try { s.execute(bad); assert(false); } catch (const Error&) {}
        assert(s.execute("(get-value (b))").back().text == "((b true))");
        assert(s.encoded_assertion_count() == 2);
    }
    Session copy = s;
    // Fill the remaining scope capacity without replacing the live encoding.
    s.execute("(push 1022)");
    assert(s.last_result().model.empty() && s.encoded_assertion_count() == 2);
    assert(s.execute("(check-sat)").back().text == "sat");
    try {
        s.execute("(push 1)");
        assert(false);
    } catch (const Error& error) {
        assert(error.category() == ErrorCategory::resource_limit);
    }
    assert(s.last_result().model.at("b") && s.encoded_assertion_count() == 2);
    s.execute("(pop 1022)");
    assert(s.last_result().model.empty() && s.encoded_assertion_count() == 2);
    s.execute("(pop 2)");
    assert(s.encoded_assertion_count() == 1);
    assert(s.execute("(check-sat-assuming ((not b)))").back().text == "sat");
    assert(copy.execute("(check-sat-assuming ((not b)))").back().text == "unsat");
    s.execute("(push 1)(declare-const c Bool)(assert c)(check-sat)");
    assert(s.last_result().model.at("c"));
    s.execute("(pop 1)(check-sat)");
    assert(!s.last_result().model.count("c"));
    s.execute("(reset-assertions)(check-sat)");
    assert(s.last_result().model.empty());
    SatLimits cap;
    cap.max_cnf_variables = 2;
    Session limited(1, cap);
    limited.execute("(declare-const a Bool)(assert (and a a))(push 1)(assert (or a a))");
    assert(limited.execute("(check-sat)").back().text == "unknown");
    limited.execute("(pop 1)");
    assert(limited.encoded_assertion_count() == 1);
    assert(limited.execute("(check-sat)").back().text == "sat");
}
