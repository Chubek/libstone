#include "Test.hpp"
#include "liblr/DiagnosticsEngine.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace firestone::lr;

static_assert(!std::is_constructible_v<LRPatchLexer, Lexer&&, const LRDiagEngine&>);
static_assert(!std::is_constructible_v<LRPatchParser, const Lexer&, LRDiagEngine&&>);

struct Fixture {
    const Grammar grammar = readEBNF("S = 'word' '+' 'word';");
    const Symbol word = grammar.findTerminal("word").value();
    const Symbol plus = grammar.findTerminal("+").value();
    const Lexer lexer = make_lexer({{"[a-z]+", word}, LexerRule::literal("+", plus), {"[ \t]+", 0, true}});
    const LLTableDriven parser{grammar};
};

LRDiagEngine makeRepairs(const Fixture& fixture, std::size_t& diagnosed) {
    LRDiagEngine engine;
    engine.addRule(lexicalLiteral("@"), [&](const LRDiagState& state) {
        ++diagnosed;
        CHECK(state.phase == LRDiagPhase::lexical);
        CHECK(state.provenance == "input.lr");
        CHECK(state.source == "\talpha@ " && state.byteBegin == 6 && state.byteEnd == 7);
        return LRDiagAction{LRDiagSeverity::warning, "replace '@'",
            LRBytePatch{state.byteBegin, state.byteEnd, "+"}, "use '+'"};
    });
    engine.addRule([](const LRDiagState& state) { return state.phase == LRDiagPhase::syntactic; },
        [&](const LRDiagState& state) {
            ++diagnosed;
            CHECK(state.provenance == "input.lr");
            CHECK(state.source == "\talpha+ " && state.byteBegin == 8 && state.byteEnd == 8);
            CHECK(state.found == fixture.grammar.eof() && state.expected == std::vector<Symbol>{fixture.word});
            CHECK(state.tokenPosition == 2 && state.tokens && state.tokens->size() == 2);
            return LRDiagAction{LRDiagSeverity::patch, "supply a missing word",
                LRTokenPatch{state.tokenPosition, state.tokenPosition, {fixture.word}}, ""};
        });
    return engine;
}

void testRecovery() {
    const Fixture fixture;
    std::size_t diagnosed = 0, cleaned = 0;
    const auto engine = makeRepairs(fixture, diagnosed);
    const std::string input = "\talpha@ ";
    const LRRecoveryOptions options{2, { [&] { ++cleaned; } }};
    const LRPatchParser patcher(fixture.lexer, engine, options);
    const auto result = patcher.parse(input, fixture.parser, "input.lr");
    CHECK(result.ok() && result.tree && result.tree->end == 3);
    CHECK(result.tokens == std::vector<Symbol>{fixture.word, fixture.plus, fixture.word});
    CHECK(result.source == "\talpha+ " && input == "\talpha@ " && result.provenance == "input.lr");
    CHECK(result.tokenBytes.size() == 3);
    CHECK(result.tokenBytes[0].begin == 1 && result.tokenBytes[0].end == 6);
    CHECK(result.tokenBytes[1].begin == 6 && result.tokenBytes[1].end == 7);
    CHECK(result.tokenBytes[2].begin == 8 && result.tokenBytes[2].end == 8);
    CHECK(result.diagnostics.size() == 2 && diagnosed == 2 && cleaned == 0);
    CHECK(result.diagnostics[0].applied && result.diagnostics[1].applied);

    const LRPatchParser bounded(fixture.lexer, engine, {1, options.resources});
    const auto failure = bounded.parse(input, fixture.parser, "input.lr");
    CHECK(!failure.ok() && failure.fatal == LRRecoveryFailure::patch_limit && !failure.tree);
    CHECK(failure.tokens == std::vector<Symbol>{fixture.word, fixture.plus});
    CHECK(failure.diagnostics.size() == 2 && failure.diagnostics[0].applied && !failure.diagnostics[1].applied);
    CHECK(cleaned == 1);
}

void testRestartAndStop() {
    const Fixture fixture;
    std::size_t cleaned = 0;
    const LRRecoveryOptions options{3, { [&] { ++cleaned; } }};
    LRDiagEngine engine;
    engine.addRule(lexicalLiteral("@"), [](const LRDiagState& state) {
        return LRDiagAction{LRDiagSeverity::patch, "", LRBytePatch{state.byteBegin, state.byteEnd, ""}, ""};
    });
    const auto merged = LRPatchLexer(fixture.lexer, engine, options).lex("ab@cd");
    CHECK(merged.ok() && merged.source == "abcd" && merged.tokens == std::vector<Symbol>{fixture.word});
    CHECK(merged.tokenBytes[0].begin == 0 && merged.tokenBytes[0].end == 4);

    LRDiagEngine noProgress;
    noProgress.addRule(lexicalLiteral("@"), [](const LRDiagState& state) {
        return LRDiagAction{LRDiagSeverity::patch, "", LRBytePatch{state.byteBegin, state.byteEnd, "@"}, ""};
    });
    const auto stuck = LRPatchLexer(fixture.lexer, noProgress, options).lex("@");
    CHECK(stuck.fatal == LRRecoveryFailure::no_progress && stuck.source == "@" && cleaned == 1);
    CHECK(stuck.diagnostics.size() == 1 && !stuck.diagnostics.front().applied);

    LRDiagEngine advice;
    advice.addRule([](const LRDiagState&) { return true; }, [](const LRDiagState&) {
        return LRDiagAction{LRDiagSeverity::inform, "invalid input", {}, "check the next symbol"};
    });
    const auto skipped = LRPatchLexer(fixture.lexer, advice, options).lex("a@b");
    CHECK(skipped.ok() && skipped.tokens == std::vector<Symbol>{fixture.word, fixture.word} && cleaned == 1);
    const auto failed = LRPatchParser(fixture.lexer, advice, options).parse("a", fixture.parser);
    CHECK(failed.fatal == LRRecoveryFailure::unpatched && cleaned == 2);
}

void testCallbackContract() {
    const Fixture fixture;
    const LRDiagEngine engine;
    std::size_t cleaned = 0;
    const LRPatchParser patcher(fixture.lexer, engine, {16, { [&] { ++cleaned; } }});
    const auto invalid = patcher.parse("a", fixture.grammar, [&](const std::vector<Symbol>& tokens) -> LRParseAttempt {
        return LLSyntaxError{tokens.size() + 1, fixture.grammar.eof(), {}};
    });
    CHECK(invalid.fatal == LRRecoveryFailure::invalid_parse_position && cleaned == 1);
    throws<std::runtime_error>([&] {
        patcher.parse("a", fixture.grammar, [](const std::vector<Symbol>&) -> LRParseAttempt {
            throw std::runtime_error("custom parser failed");
        });
    });
    const auto badLexer = make_lexer({{"a", fixture.grammar.eof()}});
    bool called = false;
    throws<std::invalid_argument>([&] {
        LRPatchParser(badLexer, engine).parse("a", fixture.grammar,
            [&](const std::vector<Symbol>&) -> LRParseAttempt {
                called = true;
                return ParseNode{};
            });
    });
    CHECK(!called);
    CHECK(cleaned == 1);
}

} // namespace

int main() { return test_main([] {
    testRecovery();
    testRestartAndStop();
    testCallbackContract();
}); }
