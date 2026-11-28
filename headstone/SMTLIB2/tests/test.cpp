#ifdef NDEBUG
#undef NDEBUG
#endif
#include <smtlib2/smtlib2.hpp>
#include <cassert>
#include <iostream>
#include <string>
int other_translation_unit();
using namespace smtlib2;
int main() {
    assert(other_translation_unit() == 0);
    auto expressions = parse(";comment\n(a \"a\"\"b\" |sym bol| ())");
    assert(expressions.size() == 1 && print(expressions[0]) == "(a \"a\"\"b\" |sym bol| ())");
    assert(expressions[0].span.begin.line == 2);
    for (const auto* bad : {"(", ")", "\"bad", "|bad", "x\"y"}) {
        bool failed = false; try { parse(bad); } catch (const Error&) { failed = true; } assert(failed);
    }
    Limits l; l.max_depth = 2; bool depth = false; try { parse("(((x)))", l); } catch (const Error&) { depth = true; } assert(depth);
    Session s;
    auto r = s.execute("(set-logic QF_UF) (declare-const x Bool) (assert x) (check-sat) (get-model) (push 1) (assert (not x)) (check-sat) (pop 1) (check-sat)");
    assert(r.size() == 4 && r[0].text == "sat" && r[1].text.find("x () Bool true") != std::string::npos && r[2].text == "unsat" && r[3].text == "sat");
    bool failed = false; try { s.execute("(assert (= 1 2))"); } catch (const Error&) { failed = true; } assert(failed);
    assert(s.execute("(get-model)")[0].text.find("x () Bool true") != std::string::npos);
    s.execute("(reset-assertions) (declare-const x Bool) (assert x)"); assert(s.execute("(check-sat)")[0].text == "sat");
    failed = false; try { s.execute("(pop 1)"); } catch (const Error&) { failed = true; } assert(failed);
    assert(s.execute("(check-sat)")[0].text == "sat");
    Session capped(0); assert(capped.execute("(declare-const x Bool) (check-sat)")[0].text == "unknown");
    Session unsound; assert(unsound.execute("(assert false) (check-sat)")[0].text == "unsat");
    failed = false; try { unsound.execute("(assert (f true))"); } catch (const Error&) { failed = true; } assert(failed);
    assert(unsound.last_result().status == CheckResult::unsat);
    std::cout << "SMTLIB2 tests passed\n";
}
