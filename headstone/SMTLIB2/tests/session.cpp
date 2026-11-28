#ifdef NDEBUG
#undef NDEBUG
#endif
#include <smtlib2/session.hpp>
#include <cassert>
#include <string>

using namespace smtlib2;

namespace {
Error rejects(Session& session, std::string_view script) {
    try {
        session.execute(script);
    } catch (const Error& error) {
        return error;
    }
    throw std::runtime_error("expected command to fail");
}

SExpr one(std::string_view text) {
    auto expressions = parse(text);
    assert(expressions.size() == 1);
    return std::move(expressions.front());
}

void command_state_and_diagnostics() {
    Session session;
    assert(rejects(session, "(set-logic Missing)").span().begin.offset == 11);
    assert(rejects(session, "(set-option :print-success |true|)").category() == ErrorCategory::syntax);
    assert(rejects(session, "(define-fun bad ((x Bool)) Bool (forall x x))").category()
           == ErrorCategory::syntax);
    const auto info = session.execute("(set-info :smt-lib-version 2.60) (set-info :note (let :example))"
                                      "(set-option :print-success true) (set-logic |QF_UF|)");
    assert(info.size() == 2 && info[0].kind == ResponseKind::success && info[1].text == "success");

    auto reply = session.execute("(declare-const p |Bool|) (assert p) (check-sat) (get-value (|p|))");
    assert(std::get<CheckResult>(reply[2].payload) == CheckResult::sat);
    assert(std::get<ValuesResponse>(reply[3].payload).bindings[0].value);
    assert(rejects(session, R"((set-option :produce-models false))").span().begin.offset == 12);
    assert(rejects(session, R"((assert (! p :named)))").category() != ErrorCategory::syntax);
    assert(rejects(session, "(definitely-not-a-command)").category() == ErrorCategory::syntax);
    assert(rejects(session, "(get-value ())").category() == ErrorCategory::syntax);
    assert(rejects(session, "(push 1025)").category() == ErrorCategory::resource_limit);
    assert(rejects(session, "(push 184467440737095516161)").category() == ErrorCategory::resource_limit);
    assert(rejects(session, "(pop 1)").span().begin.offset == 5);
    session.execute("(set-info :status |unsat|) (push 0) (pop 0)");
    assert(session.last_result().status == CheckResult::sat && session.last_result().model.at("p"));

    // Dispatch errors stop the batch, retaining successful preceding commands.
    const auto bad = rejects(session, "(push 1) (declare-const q Bool) (assert ()) (assert false)");
    assert(bad.category() == ErrorCategory::syntax && bad.span().begin.offset == 40);
    assert(session.execute("(assert q) (check-sat)").back().text == "sat");
    session.execute("(pop 1)");
    rejects(session, "(assert q)");
    rejects(session, "(get-model)");
    assert(session.last_result().model.empty());

    // Lexing and concrete parsing still complete before any dispatch.
    assert(rejects(session, "(declare-const r Bool) (").category() == ErrorCategory::syntax);
    rejects(session, "(assert r)");
    const auto ended = session.execute("(reset) (get-option :print-success) (set-logic ALL) (exit)");
    assert(ended.size() == 2 && ended[0].text == "success" && ended[1].text == "false");
    assert(session.exited());
    rejects(session, "(check-sat)");
}

void assumptions_and_resets() {
    Session session;
    session.execute("(set-option :global-declarations true) (set-option :interactive-mode true)"
                    "(set-option :produce-unsat-assumptions true) (declare-const p Bool)"
                    "(push 1) (define-fun alias () Bool |p|) (assert (not p))");
    auto replies = session.execute("(get-option :produce-assertions) (get-assertions)"
                                   "(check-sat-assuming (|alias|)) (get-unsat-assumptions)");
    assert(std::get<OptionResponse>(replies[0].payload).value);
    assert(replies[1].text == "((not p))");
    assert(std::get<CheckResult>(replies[2].payload) == CheckResult::unsat);
    assert(replies[3].text == "(|alias|)");
    assert(std::get<UnsatAssumptionsResponse>(replies[3].payload).assumptions[0].atom == "|alias|");
    rejects(session, "(check-sat-assuming (alias missing))");
    assert(rejects(session, "(check-sat-assuming ((or p alias)))").category() == ErrorCategory::syntax);
    assert(session.execute("(get-unsat-assumptions)").back().text == "(|alias|)");

    session.execute("(pop 1)");
    rejects(session, "(get-unsat-assumptions)");
    replies = session.execute("(get-assertions) (check-sat-assuming (alias)) (get-model)");
    assert(replies[0].text == "()" && replies[1].text == "sat");
    const auto& model = std::get<ModelResponse>(replies[2].payload).bindings;
    assert(model.size() == 1 && model.at("p"));
    rejects(session, "(get-unsat-assumptions)");

    replies = session.execute("(assert false) (check-sat) (get-unsat-assumptions)"
                              "(reset-assertions) (get-assertions) (check-sat-assuming (alias))");
    assert(replies[0].text == "unsat" && replies[1].text == "()" && replies[2].text == "()"
           && replies[3].text == "sat");
    rejects(session, "(set-logic ALL)");
    session.execute("(reset) (set-option :produce-models false) (check-sat)");
    assert(session.last_result().status == CheckResult::sat && session.last_result().model.empty());
    rejects(session, "(get-model)");
    rejects(session, "(get-value (true))");
    rejects(session, "(get-assertions)");
    rejects(session, "(assert alias)");
    rejects(session, "(assert p)");

    Session limited(0);
    rejects(limited, "(get-info :reason-unknown)");
    replies = limited.execute("(declare-const p Bool) (check-sat) (get-info :reason-unknown)");
    assert(replies[0].text == "unknown" && replies[1].text == "(:reason-unknown incomplete)");
    rejects(limited, "(get-model)");
    limited.execute("(reset-assertions)");
    rejects(limited, "(get-info :reason-unknown)");
    assert(limited.execute("(check-sat)").back().text == "sat");
}

void repeated_commands_and_exit() {
    for (auto logic : {"QF_UF", "QF_BV"}) {
        Session session;
        session.execute("(set-option :print-success true)(set-logic " + std::string(logic) +
                        ")(declare-const x Bool)(assert x)");
        const auto values = [&] { return session.execute("(get-value (x))").back().text; };
        assert(session.execute("(check-sat)(check-sat)").back().text == "sat");
        for (auto bad : {"(set-logic QF_UF)", "(declare-const |x| Bool)",
                         "(assert false extra)", "(check-sat-assuming (x missing))",
                         "(get-value (missing))", "(pop 1)", "(push 1025)", "(exit extra)"}) {
            rejects(session, bad);
            assert(!session.exited() && values() == "((x true))");
        }
        session.execute("(push 0)(pop 0)(set-info :status unsat)");
        assert(values() == "((x true))");
        session.execute("(reset-assertions)(reset-assertions)");
        rejects(session, "(get-model)");
        rejects(session, "(assert x)");
        session.execute("(declare-const x Bool)(assert x)(check-sat)");
        assert(values() == "((x true))");
        const auto later = rejects(session, "(exit)(reset)");
        assert(later.span().begin.offset == 6);
        assert(session.exited() && session.last_result().status == CheckResult::unknown
               && session.last_result().model.empty() && !session.last_bv_result()
               && session.encoded_assertion_count() == 0);
        rejects(session, "(exit)");
        rejects(session, "(reset)");
        assert(session.execute("; no command\n").empty());
    }
}

void check_assumptions_do_not_survive_a_new_result() {
    for (auto logic : {"QF_UF", "QF_BV"}) {
        Session session;
        session.execute("(set-option :produce-unsat-assumptions true)(set-logic " +
                        std::string(logic) + ")(declare-const x Bool)(assert x)");
        assert(session.execute("(check-sat-assuming ((not x)))(get-unsat-assumptions)")
                   .back().text == "((not x))");
        rejects(session, "(check-sat-assuming ((not x) missing))");
        assert(session.execute("(get-unsat-assumptions)").back().text == "((not x))");
        assert(session.execute("(check-sat-assuming ())(get-value (x))").back().text == "((x true))");
        rejects(session, "(get-unsat-assumptions)");
        session.execute("(assert (not x))");
        rejects(session, "(get-model)");
        assert(session.last_result().model.empty() && !session.last_bv_result());
        assert(session.execute("(check-sat)(get-unsat-assumptions)").back().text == "()");
    }
}

void scoped_boolean_terms() {
    Session session;
    session.execute("(declare-const x Bool) (assert x)"
                    "(define-fun capture ((a Bool)) Bool (and a x))"
                    "(define-fun pass ((x Bool)) Bool (capture x))"
                    "(assert (let ((x false) (y |x|)) (and (not x) y (capture true))))"
                    "(assert (not (pass false)))"
                    "(assert (let ((x false)) (let ((y x) (x |x|)) (not (or x y)))))");
    assert(session.execute("(check-sat)").back().text == "sat");
    assert(rejects(session, "(assert (let ((p true) (|p| false)) p))").span().begin.offset == 24);
    assert(rejects(session, "(assert (let ((a true) (b a)) b))").span().begin.offset == 26);
    assert(rejects(session, "(assert (let ((|true| false)) true))").span().begin.offset == 15);
    assert(rejects(session, "(define-fun replace ((|not| Bool)) Bool |not|)").span().begin.offset == 22);
    rejects(session, "(assert (replace true))");
    rejects(session, "(assert (let ((capture true)) (capture false)))");
    rejects(session, "(define-fun bad ((p Bool) (|p| Bool)) Bool p)");
    rejects(session, "(define-fun rec ((p Bool)) Bool (rec p))");
    assert(session.last_result().status == CheckResult::sat);
    session.execute("(push 1) (define-fun scoped () Bool x) (assert scoped) (pop 1)"
                    "(declare-const |scoped| Bool) (assert (not scoped))");
    assert(session.execute("(check-sat)").back().text == "sat");
    session.execute("(reset-assertions) (declare-const |let| Bool) (assert |let|)");
    assert(session.execute("(check-sat) (get-model)").back().text.find("(define-fun |let| () Bool true)")
           != std::string::npos);
}

void normalization_bounds_and_templates() {
    Session session;
    session.execute("\n\n(define-fun dup ((b Bool)) Bool (and b b)) (check-sat)");
    std::string term = "true";
    for (int i = 0; i != 18; ++i) term = "(dup " + term + ")";
    const auto error = rejects(session, "(assert " + term + ")");
    assert(error.category() == ErrorCategory::resource_limit);
    assert(error.span().begin.line == 1 && error.span().begin.offset >= 8);
    assert(session.last_result().status == CheckResult::sat);

    Limits larger;
    larger.max_depth = 600;
    const std::string nested = "(assert " + std::string(513, '(') + "true" + std::string(513, ')') + ")";
    try {
        session.execute(nested, larger);
        throw std::runtime_error("typed syntax depth limit not enforced");
    } catch (const Error& e) {
        assert(e.category() == ErrorCategory::resource_limit);
    }

    BoolEnvironment env{{"b", {}}, {"f", {{}, one("b")}}};
    assert(print(normalize_bool(one("(let ((b false)) f)"), env)) == "b");
    for (auto bad : {"absent", "f", "(unknown b)", "(not b b)", "(true b)", "99"}) {
        env.at("f").body = one(bad);
        try {
            normalize_bool(one("f"), env);
            throw std::runtime_error("malformed template normalized");
        } catch (const Error& e) {
            assert(e.span().begin.offset == 0 && e.span().end.offset == 1);
        }
    }
    try {
        normalize_bool(one("(not true)"), {}, {}, BoolNormalizationLimits{1, 512, 1000});
        throw std::runtime_error("template normalization limit not enforced");
    } catch (const UnsupportedBool& e) {
        assert(e.category() == ErrorCategory::resource_limit);
    }
}
} // namespace

int main() {
    command_state_and_diagnostics();
    assumptions_and_resets();
    repeated_commands_and_exit();
    check_assumptions_do_not_survive_a_new_result();
    scoped_boolean_terms();
    normalization_bounds_and_templates();
}
