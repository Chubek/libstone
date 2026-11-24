#include "liblr/LR.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using namespace firestone::lr;

#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)

template<class Exception = std::invalid_argument, class Callable>
void expectError(Callable test, std::string_view message) {
    try {
        test();
    } catch (const Exception& error) {
        CHECK(std::string_view(error.what()).find(message) != std::string_view::npos);
        return;
    }
    throw std::runtime_error("expected exception containing: " + std::string(message));
}

void testByteGrammarAndLexer() {
    const auto grammar = readEBNF("# header\rS = '\\0\\xFF' '[]|*?'; # trailer\r\n");
    const char raw[] = {'\0', static_cast<char>(0xff)};
    const std::string_view bytes(raw, sizeof raw);
    const auto binary = grammar.findTerminal(bytes).value();
    const auto punctuation = grammar.findTerminal("[]|*?").value();
    const std::string input = " \t" + std::string(bytes) + "[]|*?";
    for (const bool minimized : {false, true}) {
        const auto lexer = LexerGenerator::build({
            LexerRule::literal(bytes, binary),
            LexerRule::literal("[]|*?", punctuation),
            {"[ \t]+", grammar.eof(), true},
            {".", grammar.eof(), true}
        }, minimized);
        const auto symbols = std::get<std::vector<Symbol>>(lexSymbols(lexer, grammar, input));
        CHECK(symbols == std::vector<Symbol>{binary, punctuation});
        CHECK(LLTableDriven(grammar).parse(symbols));
        CHECK(LRParser(grammar).accepts(symbols));
        CHECK(ATN(grammar).accepts(symbols));
    }

    const auto lexer = make_lexer({LexerRule::literal("ab", binary), {"a.", punctuation}, {" ", 0, true},
                                   {"abc", punctuation}});
    std::size_t offset = 0;
    CHECK(lexer.next("ab abc?", offset) == LexToken{LexStatus::Token, binary, 0, 2});
    CHECK(lexer.next("ab abc?", offset) == LexToken{LexStatus::Token, punctuation, 3, 3});
    CHECK(std::get<LexToken>(lexSymbols(lexer, grammar, "ab abc?")) == LexToken{LexStatus::Error, 0, 6, 1});
    for (const auto id : {grammar.start(), grammar.eof(), grammar.symbols().size()}) {
        expectError([&] { lexSymbols(make_lexer({{"x", id}}), grammar, "x"); },
                    "invalid grammar terminal " + std::to_string(id) + " at byte 0");
    }

    // Quote all byte values, including every regex metacharacter and high byte.
    std::string alphabet;
    for (unsigned i = 0; i < 256; ++i) alphabet.push_back(static_cast<char>(i));
    std::string different = alphabet;
    different.back() = 'X';
    const auto exact = make_lexer({LexerRule::literal(alphabet, 7)});
    offset = 0;
    CHECK(exact.next(alphabet, offset) == LexToken{LexStatus::Token, 7, 0, 256});
    offset = 0;
    CHECK(exact.next(different, offset) == LexToken{LexStatus::Error, 0, 0, 1});

    expectError([&] { LexerGenerator::build({{"x", 0}, {"((y?)*)+", 1, true},
                                           LexerRule::literal("", 2)}); }, "rule priority 1");
    expectError([&] { make_lexer({LexerRule::literal("", 1)}); }, "rule priority 0");
    expectError([&] { Lexer(determinize(compile_regex("x*"))); }, "empty match");
    expectError<RegexError>([&] { make_lexer({{"\\q", 1}}); }, "regex byte");

    const std::string nulComment = std::string("S = ; #") + '\0' + "ignored";
    expectError([&] { readEBNF(nulComment); }, "EBNF byte 7: unexpected NUL");
    for (auto text : {R"(S = '\x';)", R"(S = '\xF';)", R"(S = '\x0g';)", R"(S = '\x24end';)"}) {
        expectError([&] { readEBNF(text); }, "EBNF byte");
    }
}

void testATNBranchMetadata() {
    Grammar grammar;
    const auto token = grammar.terminal("x");
    const auto s = grammar.nonterminal("S");
    const auto a = grammar.nonterminal("A");
    grammar.setStart(s);
    const auto epsilonA = grammar.addProduction(a, {});
    const auto root = grammar.addProduction(s, {a, token});
    const auto nullableRoot = grammar.addProduction(s, {});
    const auto duplicateEmpty = grammar.addProduction(s, {});
    const ATN atn(grammar);
    CHECK(!atn.entry(token) && !atn.exit(token) && !atn.exit(grammar.eof()));
    CHECK(atn.entry(s) != atn.exit(s) && atn.exit(s) != atn.exit(a));
    CHECK(atn.productionEntry(epsilonA) == atn.exit(a));
    CHECK(atn.productionEntry(nullableRoot) == atn.exit(s));
    CHECK(atn.productionEntry(duplicateEmpty) == atn.exit(s));
    const auto& branches = atn.states().at(atn.entry(s).value());
    CHECK(branches.size() == 3 && branches[1].target == branches[2].target);
    CHECK(branches[0].kind == ATN::ArcKind::epsilon && branches[0].target == atn.productionEntry(root));
    const auto& call = atn.states().at(atn.productionEntry(root)).front();
    CHECK(call.kind == ATN::ArcKind::call && call.symbol == a);
    const auto& match = atn.states().at(call.target).front();
    CHECK(match.kind == ATN::ArcKind::match && match.symbol == token && match.target == atn.exit(s));
    CHECK(atn.states().at(atn.exit(s).value()).front().kind == ATN::ArcKind::return_);
    expectError<std::out_of_range>([&] { atn.exit(grammar.symbols().size()); }, "");
    expectError<std::out_of_range>([&] { atn.productionEntry(grammar.productions().size()); }, "");
    const auto parsed = atn.parse({token});
    CHECK(parsed && parsed->children.size() == 2 && parsed->children[0].symbol == a);
    CHECK(atn.accepts({}) && !atn.accepts({token, token}));
}

void testPredictiveDiagnostics() {
    const auto grammar = readEBNF("S = 'a' Item 'z' | 'b'; Item = 'x' | ;");
    const auto a = grammar.findTerminal("a").value();
    const auto b = grammar.findTerminal("b").value();
    const auto x = grammar.findTerminal("x").value();
    const auto z = grammar.findTerminal("z").value();
    const auto eof = grammar.eof();
    const LLTableDriven parser(grammar);
    const auto result = parser.parseWithDiagnostics({a, z});
    const auto& root = std::get<ParseNode>(result);
    CHECK(root.end == 2 && root.children[1].begin == 1 && root.children[1].end == 1);
    for (const auto& input : {std::vector<Symbol>{}, {a, a}, {a, x}, {b, b}}) {
        CHECK(!parser.parse(input));
        const auto error = std::get<LLSyntaxError>(parser.parseWithDiagnostics(input));
        if (input.empty()) {
            CHECK(error.position == 0 && error.found == eof && error.expected == std::vector<Symbol>{a, b});
        } else if (input.front() == b) {
            CHECK(error.position == 1 && error.found == b && error.expected == std::vector<Symbol>{eof});
        } else if (input.back() == x) {
            CHECK(error.position == 2 && error.found == eof && error.expected == std::vector<Symbol>{z});
        } else {
            CHECK(error.position == 1 && error.found == a && error.expected == std::vector<Symbol>{z, x});
        }
    }
    expectError([&] { parser.parseWithDiagnostics({eof}); }, "invalid input terminal at token 0");
}

void testSLRTablesAndRecognition() {
    // LALR(1), but not SLR(1): FOLLOW(R) supplies a reduce action on '='.
    const auto assignment = readEBNF("S = L '=' R | R; L = '*' R | 'id'; R = L;");
    const auto eq = assignment.findTerminal("=").value();
    const auto slr = generateLRTable(assignment, LRMode::slr1);
    CHECK(slr.mode() == LRMode::slr1 && slr.conflicts().size() == 1);
    CHECK(generateLRTable(assignment, LRMode::lalr1).conflicts().empty());
    const auto& conflict = slr.conflicts().front();
    CHECK(conflict.lookahead == eq && conflict.actions.size() == 2);
    CHECK(conflict.actions[0].kind == LRActionKind::shift && conflict.actions[1].kind == LRActionKind::reduce);
    CHECK(slr.actionRow(conflict.state).at(eq) == conflict.actions);
    CHECK(slr.gotoRow(0).at(assignment.start()) == slr.goTo(0, assignment.start()));
    expectError([&] { (void)LRParser(slr); }, "on =: shift to ");
    expectError([&] { generateLRTable(assignment, static_cast<LRMode>(-1)); }, "unknown LR table mode");
    expectError<std::out_of_range>([&] { slr.actionRow(slr.stateCount()); }, "");

    // SLR drops dead alternatives before building LR(0) states.
    const auto dead = readEBNF("S = A Dead | 'z'; A = 'x'; Dead = Dead;");
    const auto x = dead.findTerminal("x").value();
    const auto z = dead.findTerminal("z").value();
    CHECK(generateLRTable(dead).actions(0, x).empty());
    const auto deadSLR = generateLRTable(dead, LRMode::slr1);
    CHECK(deadSLR.actions(0, x).empty());
    const auto afterZ = deadSLR.actionRow(0).at(z).front().target;
    CHECK(deadSLR.actions(afterZ, dead.eof()) == std::vector<LRAction>{{LRActionKind::reduce, 1}});

    // An unreachable rule must not supply FOLLOW(A) with 'a'. Otherwise
    // this conflict-free SLR table would cycle reducing A -> B -> A on x,a.
    const auto unreachable = readEBNF(
        "S = A Dead | 'z'; A = B; B = A | 'x'; Dead = Dead 'k'; Unused = A 'a';");
    const auto ux = unreachable.findTerminal("x").value();
    const auto ua = unreachable.findTerminal("a").value();
    const auto uz = unreachable.findTerminal("z").value();
    const LRParser onlyZ(unreachable, LRMode::slr1);
    CHECK(onlyZ.accepts({uz}));
    CHECK(onlyZ.table().actions(0, ux).empty());
    CHECK(!onlyZ.accepts({ux, ua}));

    // A reachable but unproductive suffix must not cause an endless chain
    // of empty reductions on the FIRST terminal of a dead production.
    const auto unproductive = readEBNF("S = A S A 'x'; A = ;");
    const auto never = LRParser(unproductive, LRMode::slr1);
    CHECK(!never.accepts({}) && !never.parse({unproductive.findTerminal("x").value()}));

    // All three modes agree with the expected language, including epsilon,
    // reductions through a nullable suffix, and complete-input recognition.
    const auto nullable = readEBNF("S = 'a' A B 'c' | ; A = 'x' | ; B = 'b' | ;");
    const auto a = nullable.findTerminal("a").value();
    const auto b = nullable.findTerminal("b").value();
    const auto c = nullable.findTerminal("c").value();
    const auto item = nullable.findTerminal("x").value();
    for (auto mode : {LRMode::canonicalLR1, LRMode::lalr1, LRMode::slr1}) {
        const LRParser parser(nullable, mode);
        for (const auto& input : {std::vector<Symbol>{}, {a, c}, {a, item, c}, {a, b, c}, {a, item, b, c}}) {
            CHECK(parser.accepts(input));
            const auto tree = parser.parse(input);
            CHECK(tree && tree->begin == 0 && tree->end == input.size());
        }
        for (const auto& input : {std::vector<Symbol>{a}, {a, c, b}, {a, b, item, c}, {c}})
            CHECK(!parser.accepts(input) && !parser.parse(input));
        expectError([&] { parser.accepts({nullable.eof()}); }, "invalid input terminal");
    }
    const auto compact = readEBNF("S = C C; C = 'c' C | 'd';");
    CHECK(generateLRTable(compact, LRMode::slr1).stateCount() < generateLRTable(compact).stateCount());
}

} // namespace

int main() {
    try {
        testByteGrammarAndLexer();
        testATNBranchMetadata();
        testPredictiveDiagnostics();
        testSLRTablesAndRecognition();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
