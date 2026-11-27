#include "../Expansion.hpp"

#include <iostream>

namespace {
using namespace am8;

void require(bool okay, std::string_view why) {
    if (!okay) throw std::logic_error(std::string(why));
}

template<class F>
std::string rejects(F run, std::string_view part) {
    try {
        run();
    } catch (const Error& error) {
        std::string message(error.what());
        if (message.find(part) == message.npos)
            throw std::logic_error("unexpected diagnostic: " + message);
        return message;
    }
    throw std::logic_error("expected error containing " + std::string(part));
}

Module mod(std::string name, std::string_view source, std::set<std::string> exports = {},
           std::vector<Import> imports = {}) {
    auto root = parse(source, name + ".am8");
    return {std::move(name), root.span, std::move(root.items), std::move(imports),
            std::move(exports), {}};
}

Import use(std::size_t target, std::string alias = {}) {
    return {target, std::move(alias), {}};
}

std::string dump(const Program& p, std::size_t form = 0) {
    return to_sexpr(p.modules[p.root].forms.at(form)).dump();
}

void macros() {
    Program p{{mod("Lib", R"((
        (syntax (hidden $x) (+ $x 1))
        (syntax (expand twice $x) (hidden $x))
        (syntax (expand once $x) $x)
        (syntax (same $x $x) $x)))", {"expand", "same"}),
        mod("Main", R"((
        (syntax (hidden $x) (* $x 2))
        (def VALUE (L.expand twice (hidden 10)))
        (def OTHER (L.expand once (L.same 3 3)))
        (def OPAQUE (quote (L.same 1 1)))
        (def QUOTING (L.same 1 "1"))))", {}, {use(0, "L")})}, 1};
    expand(p, {});
    require(dump(p) == "(def VALUE (+ (* 10 2) 1))", "macro definition and capture scopes");
    require(dump(p, 1) == "(def OTHER 3)", "multiple alternatives select the first match");
    require(dump(p, 2) == "(def OPAQUE (quote (L.same 1 1)))", "quoted data is opaque");
    require(dump(p, 3) == "(def QUOTING (L.same 1 \"1\"))", "repeated captures preserve quoting");
    require(p.modules[0].forms.empty(), "compile-time forms removed");
    require(p.modules[0].compile_time_exports == std::set<std::string>{"expand", "same"},
            "compile-time exports are identified");
    const auto& plus = p.modules[1].forms[0].items[2].items[0];
    require(plus.origin_module == 0 && plus.span.file == "Main.am8" &&
            !plus.provenance.empty(), "literal source, origin and provenance");

    Program ambiguous{{mod("A", "((syntax (f a) (def A 1)))", {"f"}),
                       mod("B", "((syntax (f b) (def B 2)))", {"f"}),
                       mod("Both", "((f a))", {}, {use(0), use(1)})}, 2};
    rejects([&] { expand(ambiguous, {}); }, "ambiguous macro 'f'");
    Program local{{ambiguous.modules[0],
                   mod("Local", "((syntax (f b) (def B 2)) (f a))", {}, {use(0)})}, 1};
    rejects([&] { expand(local, {}); }, "ambiguous macro 'f'");
    Program qualified{{ambiguous.modules[0],
                       mod("Qualified", "((f a))", {}, {use(0, "A")})}, 1};
    rejects([&] { expand(qualified, {}); }, "unknown language form f");
    Program nonreexport{{ambiguous.modules[0], mod("Middle", "()", {"f"}, {use(0)}),
                         mod("Root", "((M.f a))", {}, {use(1, "M")})}, 2};
    rejects([&] { expand(nonreexport, {}); }, "unknown language form M.f");

    Program diagnostic{{mod("Origin", "((syntax (broken) (def NAME)))", {"broken"}),
                        mod("Call", "((O.broken))", {}, {use(0, "O")})}, 1};
    const auto error = rejects([&] { expand(diagnostic, {}); }, "wrong arity");
    require(error.find("Call.am8:1:2:") != error.npos &&
            error.find("Origin.am8:1:") != error.npos &&
            error.find("expanded from here") != error.npos, "diagnostic expansion chain");
}

void stage() {
    Program p{{mod("Tools", R"((
        (stage-bind saved 5)
        (stage-fn maker (x)
          (lambda (nm) (quasiquote (def (splice nm) (splice x)))))
        (stage-fn hidden () saved)
        (stage-fn defaults (nm) ((maker (hidden)) nm))
        (stage-bind later 17)))", {"maker", "defaults"}),
        mod("Main", R"((
        (stage-bind wrap (T.maker 9))
        (stage-bind inner (list (wrap (quote FROM_CLOSURE)) (T.defaults (quote DEFAULT))))
        (stage (quasiquote (declarations (splice-list inner) (def INLINE 3))))
        (stage (declarations
          (quote (stage-bind deep 7))
          (quote (stage (quasiquote (def DEEP (splice deep)))))))
        (stage (quasiquote (stage (quasiquote (def NESTED (splice deep))))))
        (stage (let (a (b _)) (quote (2 (3 4)))
          (if (equal? b 3) (list (quote def) (quote LEGACY) a) never)))))",
        {}, {use(0, "T")})}, 1};
    expand(p, {});
    require(dump(p) == "(def FROM_CLOSURE 9)", "stage lexical closure");
    require(dump(p, 1) == "(def DEFAULT 5)", "imported stage function sees private lexical bindings");
    require(dump(p, 2) == "(def INLINE 3)", "quasiquote splice-list");
    require(dump(p, 3) == "(def DEEP 7)", "sequential staged declarations and bindings");
    require(dump(p, 4) == "(def NESTED 7)", "nested quasiquote keeps the outer splice inert");
    require(dump(p, 5) == "(def LEGACY 2)", "legacy lexical meta syntax");

    Program bad{{mod("Bad", R"((
        (stage-fn capture () later)
        (stage-bind later 17)
        (stage (capture))))")}, 0};
    const auto err = rejects([&] { expand(bad, {}); }, "unbound stage variable 'later'");
    require(err.find("stage function called here") != err.npos, "stage call trace");
    Program outside{{mod("Bad", "((stage (splice 1)))")}, 0};
    rejects([&] { expand(outside, {}); }, "splice outside quasiquote");
}

void hygiene() {
    Program p{{mod("Fresh", R"((
        (syntax (temporary $n) (fn int $n () (block
          (let int (fresh temp) 2) (return (fresh temp)))))
        (c "int am8_fresh_1;")
        (def am8_fresh_2 3)
        (temporary F)
        (stage (quasiquote (declarations
          (fn int G () (block (let int (fresh temp) 3) (return (fresh temp))))
          (temporary H))))))")}, 0};
    expand(p, {});
    for (std::size_t i = 2; i < 5; ++i) {
        const auto& block = p.modules[0].forms[i].items[4];
        const auto& local = block.items[1].items[2].text;
        const auto& ref = block.items[2].items[1].text;
        require(local == ref && local == "am8_fresh_" + std::to_string(i + 1),
                "fresh names are paired per expansion and distinct across phases");
    }
}

void limits_and_native() {
    constexpr auto source = "((syntax (f $x) (def $x 1)) (f ONE))";
    Program p{{mod("A", source), mod("B", source)}, 1};
    Limits limits;
    limits.rewrites = 1;
    rejects([&] { expand(p, {}, limits); }, "compilation-wide rewrite limit exceeded");
    require(p.modules[0].forms.size() == 2, "expansion is not published on failure");
    limits.rewrites = 2;
    limits.generated_nodes = 4;
    rejects([&] { expand(p, {}, limits); }, "compilation-wide generated syntax limit exceeded");
    limits.generated_nodes = 8;
    expand(p, {}, limits);
    require(dump(p) == "(def ONE 1)", "two module rewrites within shared budget");
    Program staged{{mod("A", "((stage-bind x 1))"), mod("B", "((stage-bind x 1))")}, 1};
    limits.stage_steps = 1;
    rejects([&] { expand(staged, {}, limits); }, "compilation-wide stage step limit exceeded");
    limits.expansion_work = 5;
    rejects([&] { expand(staged, {}, limits); }, "compilation-wide expansion work limit exceeded");

    Program loop{{mod("Loop", "((stage-fn loop () (loop)) (stage (loop)))")}, 0};
    limits = {};
    limits.stage_steps = 10;
    rejects([&] { expand(loop, {}, limits); }, "compilation-wide stage step limit exceeded");
    Program phases{{mod("Loop", "((syntax (f) (stage (quote (f)))) (f))")}, 0};
    limits = {};
    limits.rewrites = 3;
    rejects([&] { expand(phases, {}, limits); }, "compilation-wide rewrite limit exceeded");

    auto registry = NativeRegistry{};
    int calls = 0;
    registry.add({"native-def", 1, 1, {}, [&](const Syntax& x) {
        require(x.items[1].is_atom && x.items[1].text == "RESULT",
                "native argument macro-expanded");
        ++calls;
        return std::vector{parse("(def RESULT 3)", "")};
    }, {"test-only"}});
    Program native{{mod("Host", "((native native-def) (syntax (name) RESULT) (native-def (name)))")}, 0};
    expand(native, registry);
    require(dump(native) == "(def RESULT 3)" && calls == 1, "native bridge");
    Program inactive{{mod("Host", "((native-def RESULT))")}, 0};
    rejects([&] { expand(inactive, registry); }, "unknown language form native-def");
    Program invalid{{mod("Host", "((native native-def) (native-def))")}, 0};
    rejects([&] { expand(invalid, registry); }, "wrong arity for language form native-def");
    require(calls == 1, "wrong arity does not invoke trusted callbacks");
}
} // namespace

int main() {
    try {
        macros();
        stage();
        hygiene();
        limits_and_native();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
