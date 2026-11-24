#include "liblr/DiagnosticsEngine.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace firestone::lr;
using Expr = PEGExpression;

#define CHECK(...) do { \
    if (!(__VA_ARGS__)) throw std::runtime_error("lr_diagnostics.cpp:" + std::to_string(__LINE__) \
        + ": " #__VA_ARGS__); \
} while (false)

LRDiagState lexical(std::string_view source, std::size_t begin, std::size_t end) {
    LRDiagState state{};
    state.phase = LRDiagPhase::lexical;
    state.source = source;
    state.provenance = "fixture.arch";
    state.byteBegin = begin;
    state.byteEnd = end;
    return state;
}

struct Symbols {
    Grammar grammar;
    Symbol a = grammar.terminal("a");
    Symbol b = grammar.terminal("b");
    Symbol other = grammar.terminal("other");
    Symbol start = grammar.nonterminal("S");

    Symbols() {
        grammar.setStart(start);
        grammar.addProduction(start, {a, b});
    }

    PEG peg(Expr expression) const {
        return PEG(grammar, {{start, std::move(expression)}});
    }

    LRDiagState syntactic(const std::vector<Symbol>& tokens, std::size_t position) const {
        LRDiagState state{};
        state.phase = LRDiagPhase::syntactic;
        state.provenance = "fixture.arch";
        state.grammar = &grammar;
        state.tokens = &tokens;
        state.tokenPosition = position;
        if (position < tokens.size()) state.found = tokens[position];
        state.expected = {a, grammar.eof()};
        return state;
    }
};

void checkFatal(const LRDiagAction& action) {
    CHECK(action.severity == LRDiagSeverity::error);
    CHECK(!action.message.empty());
    CHECK(!action.patch);
}

template<class Exception, class Function>
void expectThrow(Function test) {
    try {
        test();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("expected invalid pattern to be rejected at construction");
}

template<class Pattern>
LRDiagEngine engineWith(Pattern pattern) {
    LRDiagEngine engine;
    engine.addRule(std::move(pattern), [](const LRDiagState&) {
        return LRDiagAction{LRDiagSeverity::inform, "matched", {}, {}};
    });
    return engine;
}

void testDispatch() {
    const std::string source("x\0\xff!", 4);
    const auto state = lexical(source, 1, 3);
    LRDiagEngine engine;
    CHECK(!engine.tryDiagnose(state));
    checkFatal(engine.diagnose(state));

    std::string calls;
    engine.addRule([&](const LRDiagState&) {
        calls += '1';
        return false;
    }, [](const LRDiagState&) -> LRDiagAction {
        throw std::runtime_error("action of unmatched rule ran");
    });
    engine.addRule([&](const LRDiagState& seen) {
        calls += '2';
        CHECK(seen.phase == LRDiagPhase::lexical);
        CHECK(seen.source.data() == source.data() && seen.source == source);
        CHECK(seen.provenance == "fixture.arch");
        CHECK(seen.byteBegin == 1 && seen.byteEnd == 3);
        return true;
    }, [&](const LRDiagState& seen) {
        calls += 'A';
        return LRDiagAction{LRDiagSeverity::warning, "invalid bytes",
            LRBytePatch{seen.byteBegin, seen.byteEnd, "?"}, "replace the bytes"};
    });
    engine.addRule([](const LRDiagState&) -> bool {
        throw std::runtime_error("rule after first match ran");
    }, [](const LRDiagState&) -> LRDiagAction {
        throw std::runtime_error("action after first match ran");
    });

    const auto result = engine.diagnose(state);
    CHECK(calls == "12A");
    CHECK(result.severity == LRDiagSeverity::warning && result.message == "invalid bytes");
    CHECK(result.suggestion == "replace the bytes");
    CHECK(result.patch && std::holds_alternative<LRBytePatch>(*result.patch));
    const auto& patch = std::get<LRBytePatch>(*result.patch);
    CHECK(patch.begin == 1 && patch.end == 3 && patch.replacement == "?");
    CHECK(source == std::string("x\0\xff!", 4));
}

void testLiteralAndRegex() {
    const std::string bytes("x\0\xff!", 4);
    const auto literal = engineWith(lexicalLiteral(std::string("\0\xff", 2)));
    const auto regex = engineWith(lexicalRegex(R"(\x00[\x80-\xff])"));
    for (const auto* engine : {&literal, &regex}) {
        CHECK(engine->diagnose(lexical(bytes, 1, 3)).message == "matched");
        CHECK(engine->diagnose(lexical(bytes, 1, 2)).message == "matched");
        checkFatal(engine->diagnose(lexical(bytes, 0, 3))); // no search ahead
        checkFatal(engine->diagnose(lexical(bytes, 2, 3)));
        checkFatal(engine->diagnose(lexical(bytes, 4, 4)));

        const Symbols symbols;
        const std::vector<Symbol> tokens{symbols.a};
        auto wrongPhase = symbols.syntactic(tokens, 0);
        wrongPhase.source = bytes;
        wrongPhase.byteBegin = 1;
        wrongPhase.byteEnd = 3;
        checkFatal(engine->diagnose(wrongPhase));
    }
    CHECK(engineWith(lexicalLiteral("")).diagnose(lexical(bytes, 4, 4)).message == "matched");
    CHECK(engineWith(lexicalRegex("a?")).diagnose(lexical(bytes, 4, 4)).message == "matched");
    expectThrow<RegexError>([] { (void)lexicalRegex("["); });
}

void testSyntacticPEG() {
    const Symbols s;
    const auto a = Expr::token(s.a);
    const auto b = Expr::token(s.b);
    const auto ab = Expr::sequence({a, b});
    const auto prefix = engineWith(syntacticPEG(s.peg(ab)));
    const auto shortFirst = engineWith(syntacticPEG(
        s.peg(Expr::sequence({Expr::choice({a, ab}), Expr::end()}))));
    const auto longFirst = engineWith(syntacticPEG(
        s.peg(Expr::sequence({Expr::choice({ab, a}), Expr::end()}))));
    const auto eof = engineWith(syntacticPEG(s.peg(Expr::end())));
    const std::vector<Symbol> tokens{s.other, s.a, s.b};
    CHECK(prefix.diagnose(s.syntactic(tokens, 1)).message == "matched");
    checkFatal(prefix.diagnose(s.syntactic(tokens, 0)));
    checkFatal(shortFirst.diagnose(s.syntactic(tokens, 1))); // no backtracking into a matched choice
    CHECK(longFirst.diagnose(s.syntactic(tokens, 1)).message == "matched");
    checkFatal(eof.diagnose(s.syntactic(tokens, 2)));
    CHECK(eof.diagnose(s.syntactic(tokens, 3)).message == "matched");
    checkFatal(eof.diagnose(lexical("", 0, 0)));

    const std::vector<Symbol> trailing{s.other, s.a, s.b, s.other};
    CHECK(prefix.diagnose(s.syntactic(trailing, 1)).message == "matched");
    checkFatal(longFirst.diagnose(s.syntactic(trailing, 1)));

    Grammar different;
    const auto differentA = different.terminal("different-a");
    const auto sameB = different.terminal("b");
    CHECK(differentA == s.a && sameB == s.b);
    const auto start = different.nonterminal("S");
    different.setStart(start);
    different.addProduction(start, {differentA, sameB});
    const std::vector<Symbol> foreignTokens{differentA, sameB};
    auto foreign = s.syntactic(foreignTokens, 0);
    foreign.grammar = &different;
    checkFatal(prefix.diagnose(foreign)); // equal IDs alone cannot establish compatibility
    CHECK(tokens == std::vector<Symbol>({s.other, s.a, s.b}));

    const auto tokenSet = s.peg(Expr::tokenSet({s.b, s.a}));
    const auto tree = tokenSet.parsePrefix(tokens, 2);
    CHECK(tree && tree->end == 3 && tree->children.size() == 1);
    CHECK(tree->children.front().symbol == s.b);
    CHECK(!tokenSet.matchPrefix(tokens));
    expectThrow<std::invalid_argument>([&] { (void)s.peg(Expr::tokenSet({s.a, s.grammar.eof()})); });
}

void testLexicalPEG() {
    const Symbols s;
    std::array<std::optional<Symbol>, 256> mapping{};
    mapping[0x00] = s.a;
    mapping[0xff] = s.b;
    mapping['!'] = s.other;
    const auto ab = Expr::sequence({Expr::token(s.a), Expr::token(s.b)});
    const auto prefix = engineWith(lexicalPEG(s.peg(ab), mapping));
    const auto throughEnd = engineWith(lexicalPEG(s.peg(Expr::sequence({ab, Expr::end()})), mapping));
    const auto eof = engineWith(lexicalPEG(s.peg(Expr::end()), mapping));

    const std::string bytes("x\0\xff", 3);
    CHECK(prefix.diagnose(lexical(bytes, 1, 3)).message == "matched");
    CHECK(throughEnd.diagnose(lexical(bytes, 1, 3)).message == "matched");
    CHECK(throughEnd.diagnose(lexical(bytes, 1, 2)).message == "matched");
    checkFatal(prefix.diagnose(lexical(bytes, 0, 3)));
    CHECK(eof.diagnose(lexical(bytes, 3, 3)).message == "matched");
    checkFatal(eof.diagnose(lexical(bytes, 2, 3)));

    const std::string trailing("x\0\xff!", 4);
    CHECK(prefix.diagnose(lexical(trailing, 1, 4)).message == "matched");
    checkFatal(throughEnd.diagnose(lexical(trailing, 1, 4)));
    const std::string gap("x\0?\xff", 4);
    checkFatal(prefix.diagnose(lexical(gap, 1, 4))); // an unmapped byte is not skipped
    const std::string unmappedTail("x\0\xff?", 4);
    checkFatal(throughEnd.diagnose(lexical(unmappedTail, 1, 3))); // not a synthetic EOF

    for (const auto invalid : {s.grammar.eof(), s.start, std::numeric_limits<Symbol>::max()}) {
        mapping['?'] = invalid;
        expectThrow<std::invalid_argument>([&] { (void)lexicalPEG(s.peg(ab), mapping); });
    }
}

void testBytePEGText() {
    const LRDiagBytePEG pattern(R"PEG(&[a-z] ("abc" / 'ab') [0-9]+ '-'? !.)PEG");
    CHECK(pattern.matchPrefixAt("..abc123-", 2) == 9);
    CHECK(pattern.matchPrefixAt("ab9") == 3);
    CHECK(!pattern.matchPrefixAt("..abc123-", 0)); // No scan ahead.
    CHECK(!pattern.matchPrefixAt("abc123-X"));
    CHECK(!pattern.matchPrefixAt("abc"));
    CHECK(!LRDiagBytePEG("('a' / 'ab') !.").matchPrefixAt("ab"));
    CHECK(!LRDiagBytePEG("'a'* 'a'").matchPrefixAt("a")); // Possessive.
    CHECK(LRDiagBytePEG("'a'* 'b'").matchPrefixAt("aaabx") == 4);
    CHECK(LRDiagBytePEG("! 'a' .").matchPrefixAt("b") == 1);
    CHECK(!LRDiagBytePEG("!'a' .").matchPrefixAt("a"));
    CHECK(LRDiagBytePEG("!.").matchPrefixAt("x", 1) == 1);
    CHECK(!LRDiagBytePEG("!.").matchPrefixAt("x", 0));
    CHECK(!LRDiagBytePEG("!.").matchPrefixAt("x", 2));
    CHECK(LRDiagBytePEG("''").matchPrefixAt("abc", 1) == 1);

    std::size_t remaining = 50;
    const LRDiagBytePEG bounded(".* !.");
    CHECK(bounded.matchPrefixAt("abc", 0, remaining) == 3);
    CHECK(remaining < 50);
    expectThrow<std::length_error>([&] { (void)bounded.matchPrefixAt(std::string(50, 'a'), 0, remaining); });
    CHECK(remaining == 0);
    CHECK(!bounded.matchPrefixAt("", 1, remaining));

    const std::string binary("x\0\xff\r\n", 5);
    const LRDiagBytePEG escaped(R"('\0' [\x80-\xFF] '\r\n')");
    CHECK(escaped.matchPrefixAt(binary, 1) == 5);
    CHECK(LRDiagBytePEG("[^a-z]+").matchPrefixAt(binary, 1) == 5);
    CHECK(LRDiagBytePEG(".").matchPrefixAt(binary, 1) == 2);
    CHECK(LRDiagBytePEG(R"([\]\-\\]+ !.)").matchPrefixAt("]-\\") == 3);
    CHECK(LRDiagBytePEG("[-a]+").matchPrefixAt("-aa") == 3);
    CHECK(LRDiagBytePEG("[a-]+").matchPrefixAt("a--") == 3);
    CHECK(!LRDiagBytePEG(R"([^\x00-\xff])").matchPrefixAt(binary));

    auto engine = engineWith(lexicalPEG(escaped));
    const auto action = engine.tryDiagnose(lexical(binary, 1, 2));
    CHECK(action && action->message == "matched");
    CHECK(!engine.tryDiagnose(lexical(binary, 0, 2)));

    for (const std::string_view invalid : {"", "word", "()", "('a'", "'a'/", "/'a'", "'a')",
             "'a'**", "'unclosed", "[]", "[^]", "[a-z", "[z-a]", R"('\x7')", R"('\q')",
             "''*", "'a'?+", "('a'?)*", "(&'a')+", "('' / 'a')*", "(!.)*"})
        expectThrow<LRDiagBytePEGError>([&] { (void)LRDiagBytePEG(invalid); });
    expectThrow<LRDiagBytePEGError>([] { (void)LRDiagBytePEG(std::string(1000, '!') + "."); });
    try {
        (void)LRDiagBytePEG("  [z-a]");
        CHECK(false);
    } catch (const LRDiagBytePEGError& error) {
        CHECK(error.offset() == 5);
    }
}

void testTokenPatch() {
    const Symbols s;
    const std::vector<Symbol> tokens{s.a};
    const auto state = s.syntactic(tokens, 1);
    LRDiagEngine engine;
    engine.addRule([](const LRDiagState&) { return true; }, [&](const LRDiagState& seen) {
        CHECK(seen.grammar == &s.grammar && seen.tokens == &tokens);
        CHECK(seen.tokenPosition == tokens.size() && !seen.found);
        CHECK(seen.expected == std::vector<Symbol>({s.a, s.grammar.eof()}));
        return LRDiagAction{LRDiagSeverity::patch, "insert b",
            LRTokenPatch{seen.tokenPosition, seen.tokenPosition, {s.b}}, {}};
    });
    const auto result = engine.diagnose(state);
    CHECK(result.severity == LRDiagSeverity::patch);
    CHECK(result.patch && std::holds_alternative<LRTokenPatch>(*result.patch));
    const auto& patch = std::get<LRTokenPatch>(*result.patch);
    CHECK(patch.begin == 1 && patch.end == 1 && patch.replacement == std::vector<Symbol>({s.b}));
    CHECK(tokens == std::vector<Symbol>({s.a}));
}

void testInvalidStates() {
    int calls = 0;
    const auto engine = engineWith([&](const LRDiagState&) { ++calls; return true; });
    checkFatal(engine.diagnose(lexical("xyz", 2, 1)));
    checkFatal(engine.diagnose(lexical("xyz", 0, 4)));
    checkFatal(engine.diagnose(lexical("xyz", std::numeric_limits<std::size_t>::max(), 3)));

    const Symbols s;
    const std::vector<Symbol> tokens{s.a};
    checkFatal(engine.diagnose(s.syntactic(tokens, 2)));
    auto state = s.syntactic(tokens, 0);
    state.found = s.b;
    checkFatal(engine.diagnose(state));
    state = s.syntactic(tokens, 0);
    state.expected = {s.start};
    checkFatal(engine.diagnose(state));
    const std::vector<Symbol> withExplicitEOF{s.a, s.grammar.eof()};
    checkFatal(engine.diagnose(s.syntactic(withExplicitEOF, 0)));
    CHECK(calls == 0); // validation precedes user predicates
}

void testInvalidActions() {
    const std::string source = "xyz";
    const auto bytes = lexical(source, 1, 2);
    const Symbols s;
    const std::vector<Symbol> tokens{s.a};
    const auto syntax = s.syntactic(tokens, 0);
    auto rejects = [](const LRDiagState& state, LRDiagAction action) {
        int calls = 0;
        LRDiagEngine engine;
        engine.addRule([&](const LRDiagState&) { ++calls; return true; },
            [&](const LRDiagState&) { return action; });
        engine.addRule([&](const LRDiagState&) { ++calls; return true; },
            [](const LRDiagState&) {
                return LRDiagAction{LRDiagSeverity::inform, "unexpected fallback rule", {}, {}};
            });
        checkFatal(engine.diagnose(state));
        CHECK(calls == 1); // invalid actions must not fall through to another rule
    };
    rejects(bytes, {LRDiagSeverity::patch, "", {}, {}});
    rejects(bytes, {LRDiagSeverity::silent_patch, "", {}, {}});
    rejects(bytes, {LRDiagSeverity::silent_patch, "not silent", LRBytePatch{1, 2, "!"}, {}});
    rejects(bytes, {LRDiagSeverity::silent_patch, "", LRBytePatch{1, 2, "!"}, "not silent"});
    rejects(bytes, {LRDiagSeverity::loud_patch, "", LRBytePatch{1, 2, "!"}, {}});
    rejects(bytes, {LRDiagSeverity::loud_patch, "missing edit", {}, {}});
    rejects(bytes, {LRDiagSeverity::patch, "", LRBytePatch{2, 1, ""}, {}});
    rejects(bytes, {LRDiagSeverity::warning, "bad edit", LRBytePatch{1, 4, ""}, {}});
    rejects(bytes, {LRDiagSeverity::patch, "", LRTokenPatch{0, 0, {}}, {}});
    rejects(syntax, {LRDiagSeverity::patch, "", LRBytePatch{0, 0, ""}, {}});
    rejects(syntax, {LRDiagSeverity::patch, "", LRTokenPatch{1, 0, {}}, {}});
    rejects(syntax, {LRDiagSeverity::patch, "", LRTokenPatch{0, 2, {}}, {}});
    for (const auto invalid : {s.grammar.eof(), s.start, std::numeric_limits<Symbol>::max()})
        rejects(syntax, {LRDiagSeverity::patch, "", LRTokenPatch{0, 1, {invalid}}, {}});

    LRDiagEngine engine;
    engine.addRule([](const LRDiagState&) { return true; }, [](const LRDiagState&) {
        return LRDiagAction{LRDiagSeverity::error, "fatal", LRBytePatch{1, 2, "!"},
            "edit the source"};
    });
    const auto fatal = engine.diagnose(bytes);
    checkFatal(fatal);
    CHECK(fatal.message == "fatal" && fatal.suggestion == "edit the source");
    CHECK(source == "xyz" && tokens == std::vector<Symbol>({s.a}));
}

void testExplicitPatchOutcomes() {
    for (const auto severity : {LRDiagSeverity::silent_patch, LRDiagSeverity::loud_patch}) {
        LRDiagEngine engine;
        const bool loud = severity == LRDiagSeverity::loud_patch;
        engine.addRule(lexicalPEG(LRDiagBytePEG("'!'")), [=](const LRDiagState& state) {
            return LRDiagAction{severity, loud ? "replace invalid byte" : "",
                LRBytePatch{state.byteBegin, state.byteEnd, "?"}, loud ? "try a question mark" : ""};
        });
        const auto action = engine.tryDiagnose(lexical("a!", 1, 2));
        CHECK(action && action->severity == severity && action->patch);
        CHECK(action->message.empty() != loud);
        CHECK(action->suggestion.empty() != loud);
        CHECK(!engine.tryDiagnose(lexical("a?", 1, 2)));
        const auto invalid = engine.tryDiagnose(lexical("a!", 1, 3));
        CHECK(invalid && invalid->severity == LRDiagSeverity::error);
    }
}

} // namespace

int main() {
    try {
        testDispatch();
        testLiteralAndRegex();
        testSyntacticPEG();
        testLexicalPEG();
        testBytePEGText();
        testTokenPatch();
        testInvalidStates();
        testInvalidActions();
        testExplicitPatchOutcomes();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
