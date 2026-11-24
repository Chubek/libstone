#include "liblr/DFA.hpp"
#include "liblr/DFA2Assembly.hpp"
#include "liblr/DFA2C99.hpp"
#include "liblr/DFA2SIMD.hpp"
#include "liblr/DFAMinimization.hpp"
#include "liblr/LexerGenerator.hpp"
#include "liblr/NFA.hpp"
#include "liblr/ThompsonConstruction.hpp"
#include <cstddef>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)

using namespace firestone::lr;

namespace {

template<class Error, class Callable>
void throws(Callable callable) {
    bool caught = false;
    try { callable(); } catch (const Error&) { caught = true; }
    CHECK(caught);
}

void regex_and_nfa() {
    const auto regex = determinize(compile_regex(R"((ab|c)+d?[\x00-\x02\xff])", {42, 2, false}));
    CHECK(regex.match_prefix(std::string("abcabd\0tail", 11)) == DFAMatch{7, {42, 2, false}});
    CHECK(regex.match_prefix("cc\xff") == DFAMatch{3, {42, 2, false}});
    CHECK(!regex.match_prefix("d\xff"));
    CHECK(!regex.match_prefix("ab"));
    CHECK(determinize(compile_regex("((x?)*)|")).match_prefix("xxxY") == DFAMatch{3, {}});
    CHECK(determinize(compile_regex("()")).match_prefix("") == DFAMatch{0, {}});
    CHECK(determinize(compile_regex(R"([^a-z][a\-\]]+\r\n\t)")).match_prefix("0-a]\r\n\t") == DFAMatch{7, {}});
    CHECK(determinize(compile_regex(".")).match_prefix("\n") == DFAMatch{1, {}});
    CHECK(determinize(compile_regex(std::string_view("\0", 1))).match_prefix(std::string_view("\0", 1)) ==
          DFAMatch{1, {}});
    CHECK(!determinize(compile_regex(R"([^\x00-\xff])")).match_prefix("x"));
    for (std::string_view broken : {"(", "a)", "[", "[]", "[^]", "[z-a]", R"(\x1)", R"(\q)", "\\",
                                    "a++", "*", "^a", "x{2}"})
        throws<RegexError>([&] { compile_regex(broken); });
    throws<RegexError>([&] { compile_regex(std::string(257, '(')); });
    try {
        compile_regex("a[z-a]");
        CHECK(false);
    } catch (const RegexError& error) {
        CHECK(error.offset() == 5);
    }

    NFA nfa;
    auto start = nfa.add_state(), junction = nfa.add_state();
    auto low = nfa.add_state(), high = nfa.add_state(), tie = nfa.add_state(), longer = nfa.add_state();
    nfa.set_start(start);
    nfa.add_epsilon(start, junction);
    nfa.add_epsilon(junction, start);
    ByteSet a, b;
    a.set('a');
    b.set('b');
    nfa.add_transition(start, a, low);
    nfa.add_transition(junction, a, high);
    nfa.add_epsilon(high, tie);
    nfa.add_transition(low, b, longer);
    nfa.set_acceptance(low, Acceptance{1, 10, false});
    nfa.set_acceptance(high, Acceptance{3, 3, true});
    nfa.set_acceptance(tie, Acceptance{2, 3, false});
    nfa.set_acceptance(longer, Acceptance{4, 20, false});
    auto dfa = determinize(nfa);
    CHECK(dfa.match_prefix("a?") == DFAMatch{1, {2, 3, false}});
    CHECK(dfa.match_prefix("ab?") == DFAMatch{2, {4, 20, false}});
    CHECK(!dfa.match_prefix("b"));
    throws<std::out_of_range>([&] { nfa.add_epsilon(start, no_state); });
    throws<std::out_of_range>([&] { nfa.add_transition(no_state, ByteSet{}, start); });

    NFA combined;
    const auto outer_start = combined.add_state();
    combined.set_start(outer_start);
    combined.add_alternative(nfa); // Both NFAs have nonzero starts.
    add_regex_alternative(combined, "a.", {7, 8, true});
    const auto count = combined.states().size();
    const auto alternatives = combined.states()[outer_start].epsilon;
    throws<RegexError>([&] { add_regex_alternative(combined, "(c"); });
    CHECK(combined.states().size() == count && combined.states()[outer_start].epsilon == alternatives);
    combined.add_alternative(combined);
    CHECK(combined.start() == outer_start && combined.states().size() == 2 * count);
    CHECK(nfa.states().size() == 7 && nfa.start() == start);
    const auto both = determinize(combined);
    CHECK(both.match_prefix("a") == DFAMatch{1, {2, 3, false}});
    CHECK(both.match_prefix("ab") == DFAMatch{2, {7, 8, true}});
    CHECK(both.match_prefix("a\xff") == DFAMatch{2, {7, 8, true}});
    CHECK(!both.match_prefix("b"));
}

// Exhaust the product automaton, not just a sample of input strings. The extra
// state no_state is the implicit rejecting sink with transitions to itself.
bool equivalent(const DFA& a, StateId aa, const DFA& b, StateId bb) {
    using Pair = std::pair<StateId, StateId>;
    std::vector<Pair> pending{{aa, bb}};
    std::set<Pair> seen{pending.front()};
    auto accept = [](const DFA& dfa, StateId id) {
        return id == no_state ? std::optional<Acceptance>{} : dfa.states()[id].acceptance;
    };
    auto next = [](const DFA& dfa, StateId id, std::size_t byte) {
        return id == no_state ? no_state : dfa.states()[id].transitions[byte];
    };
    for (std::size_t i = 0; i < pending.size(); ++i) {
        auto [lhs, rhs] = pending[i];
        if (!(accept(a, lhs) == accept(b, rhs))) return false;
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            Pair pair{next(a, lhs, byte), next(b, rhs, byte)};
            if (seen.insert(pair).second) pending.push_back(pair);
        }
    }
    return true;
}

void subset_budget() {
    const auto loop = compile_regex("a+");
    const auto normal = determinize(loop);
    const auto count = normal.states().size();
    CHECK(count > 1);
    const auto at_limit = determinize_bounded(loop, count);
    CHECK(at_limit.states().size() == count);
    CHECK(equivalent(normal, normal.start(), at_limit, at_limit.start()));
    throws<std::length_error>([&] { determinize_bounded(loop, count - 1); });
    throws<std::length_error>([] { determinize_bounded(NFA{}, 0); });
    CHECK(determinize_bounded(NFA{}, 1).states().size() == 1);
}

// Each byte has a unique combination of transitions across these rows.
DFA full_byte_alphabet(bool merge_last_pair = false) {
    std::vector<DFAState> states(8);
    for (StateId id = 0; id < states.size(); ++id) {
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            const auto bits = merge_last_pair && byte == 254 ? 255 : byte;
            if (bits & (1u << id)) states[id].transitions[byte] = (id + 1) % states.size();
        }
    }
    states[7].acceptance = Acceptance{54, 0, false};
    return DFA(std::move(states), 3);
}

void byte_classes() {
    const auto literal = determinize(compile_regex(R"([\x00\xff]x|[\x00\xff]z)"));
    const auto alphabet = literal.byte_classes();
    CHECK(alphabet.index[0] == alphabet.index[255]);
    CHECK(alphabet.index['x'] != alphabet.index['y']);
    CHECK(alphabet.representatives.front() == 0);
    // Compare all pairs against the definition, including state distinctions
    // invisible in the start row and the default DFA's one-class alphabet.
    for (const auto& dfa : {literal, DFA{}, full_byte_alphabet()}) {
        const auto classes = dfa.byte_classes();
        for (std::size_t a = 0; a < byte_count; ++a) {
            const auto rep = classes.representatives[classes.index[a]];
            CHECK(rep <= a && classes.index[rep] == classes.index[a]);
            for (std::size_t b = a; b < byte_count; ++b) {
                bool equal = true;
                for (const auto& state : dfa.states()) equal &= state.transitions[a] == state.transitions[b];
                CHECK((classes.index[a] == classes.index[b]) == equal);
            }
        }
        const auto result = minimize(dfa);
        CHECK(equivalent(dfa, dfa.start(), result, result.start()));
    }
    CHECK(full_byte_alphabet().byte_classes().representatives.size() == byte_count);
}

void minimization() {
    const auto unminimized = determinize(compile_regex("ab|cb"));
    const auto minimized = minimize(unminimized);
    CHECK(unminimized.states().size() > 3);
    CHECK(minimized.states().size() == 3);
    CHECK(equivalent(unminimized, unminimized.start(), minimized, minimized.start()));

    std::vector<DFAState> states(7);
    states[2].transitions['a'] = 0;
    states[2].transitions['b'] = 1;
    states[2].transitions['c'] = 3;
    states[2].transitions['d'] = 4;
    states[2].transitions['e'] = 5;
    states[0].acceptance = Acceptance{10, 0, false};
    states[1].acceptance = Acceptance{10, 0, true}; // Same token, different skip.
    states[5].acceptance = Acceptance{10, 1, false}; // Same token, different priority.
    states[3].transitions['x'] = 4;
    states[4].transitions['x'] = 3; // Both dead.
    states[6].acceptance = Acceptance{11, 2, false}; // Unreachable.
    const auto hand = DFA(states, 2);
    const auto cleaned = minimize(hand);
    CHECK(cleaned.states().size() == 4);
    CHECK(equivalent(hand, hand.start(), cleaned, cleaned.start()));
    CHECK(cleaned.states()[cleaned.start()].transitions['c'] == no_state);
    CHECK(minimize(DFA{}).states().size() == 1);
    states[0].transitions['q'] = states.size();
    throws<std::invalid_argument>([&] { DFA bad(states); });
    throws<std::invalid_argument>([&] { DFA bad({}, 2); });

    std::mt19937 random(602);
    for (unsigned int test = 0; test < 40; ++test) {
        std::vector<DFAState> sample(9);
        for (auto& state : sample) {
            if (random() % 3 == 0) state.acceptance = Acceptance{static_cast<std::size_t>(random() % 2), 0, false};
            for (unsigned char byte : {0, 13, 128, 255}) {
                auto target = random() % 12;
                if (target < sample.size()) state.transitions[byte] = target;
            }
        }
        const auto before = DFA(std::move(sample), random() % 9);
        const auto after = minimize(before);
        CHECK(equivalent(before, before.start(), after, after.start()));
        for (StateId a = 0; a < after.states().size(); ++a) {
            for (StateId b = a + 1; b < after.states().size(); ++b) CHECK(!equivalent(after, a, after, b));
            if (after.states().size() > 1) CHECK(!equivalent(after, a, after, no_state));
        }
    }
}

void lexing() {
    const auto largest = std::numeric_limits<std::size_t>::max();
    auto lexer = LexerGenerator{}.add_rule({"[ \t]+", 0, true}).add_rule({"if", 1})
        .add_rule({"[a-z]+", 2}).add_rule({"A", 3}).add_rule({"ABCD", 4})
        .add_rule({R"(\xff\0)", largest}).build();
    const std::string input = std::string(" if iffy ABC! \xff\0", 16) + " ";
    std::size_t offset = 0;
    CHECK(lexer.next(input, offset) == LexToken{LexStatus::Token, 1, 1, 2});
    CHECK(lexer.next(input, offset) == LexToken{LexStatus::Token, 2, 4, 4});
    CHECK(lexer.next(input, offset) == LexToken{LexStatus::Token, 3, 9, 1});
    CHECK(lexer.next(input, offset) == LexToken{LexStatus::Error, 0, 10, 1});
    CHECK(lexer.next(input, offset) == LexToken{LexStatus::Error, 0, 11, 1});
    CHECK(lexer.next(input, offset) == LexToken{LexStatus::Error, 0, 12, 1});
    CHECK(lexer.next(input, offset) == LexToken{LexStatus::Token, largest, 14, 2});
    CHECK(lexer.next(input, offset) == LexToken{LexStatus::End, 0, 17, 0});
    CHECK(offset == input.size());
    offset = 1;
    CHECK(lexer.next(" ABCD", offset) == LexToken{LexStatus::Token, 4, 1, 4});
    CHECK(offset == 5);
    throws<std::out_of_range>([&] { lexer.next("a", offset); });
    CHECK(offset == 5);

    const auto skipping = LexerGenerator::build({{"x", 9, true}, {"x", 0}, {"y", 0}}, false);
    offset = 0;
    CHECK(skipping.next("xxxy", offset) == LexToken{LexStatus::Token, 0, 3, 1});
    CHECK(offset == 4);
    offset = 0;
    CHECK(make_lexer({}).next("!", offset) == LexToken{LexStatus::Error, 0, 0, 1});
    CHECK(make_lexer({}).next("!", offset) == LexToken{LexStatus::End, 0, 1, 0});
    for (auto pattern : {"", "a*", "a|", "(a?)+", "(|x)"})
        throws<std::invalid_argument>([&] { make_lexer({{pattern, 0, true}}); });
}

void simd() {
    // Four disjoint loop ranges, complement-only representation, complex scalar
    // loop set, and an all-byte loop; test both accepting and nonaccepting loops.
    const auto lexer = make_lexer({{"[ \t]+", 0, true}, {"[a-zA-Z0-9_]+", 1},
                                   {R"(<[^>\n]*>)", 2}, {"[!#%')+]+", 3},
                                   {R"(\x00.+)", 4}, {R"(\x80[\x80-\xff]+)", 5}});
    const SIMDLexer native(lexer), fallback(lexer, false);
    CHECK(native.simd_enabled() == SIMDMatcher::native_supported());
    CHECK(!fallback.simd_enabled());
    std::mt19937 random(771);
    for (std::size_t n = 0; n < 65; ++n) {
        // Starts at an unaligned byte; SIMD blocks often straddle delimiters.
        auto input = "?" + std::string(n, ' ') + std::string(n + 1, 'a') + "<" +
                     std::string(n + 17, char(0xff)) + ">[" + std::string(n + 18, '!') + "]" +
                     std::string(n + 1, char(0x80)) + std::string(n + 18, char(0xff));
        for (std::size_t i = 0; i < 30; ++i) input += static_cast<char>(random() & 255);
        std::size_t a = 1, b = a, c = a;
        while (a < input.size()) {
            auto expected = lexer.next(input, a);
            CHECK(native.next(input, b) == expected);
            CHECK(fallback.next(input, c) == expected);
            CHECK(a == b && b == c);
        }
    }
    // High bytes and nullable starts at the matcher level, but not in a lexer.
    const SIMDMatcher matcher(minimize(determinize(compile_regex(R"([\x80-\xff]*)"))));
    CHECK(matcher.match_prefix(std::string(33, char(0x80)) + "x") == DFAMatch{33, {}});
    CHECK(matcher.match_prefix("x") == DFAMatch{0, {}});
    const auto all_bytes = minimize(determinize(compile_regex(".+")));
    std::string raw;
    for (std::size_t byte = 0; byte < byte_count; ++byte) raw += static_cast<char>(byte);
    CHECK(SIMDMatcher(all_bytes).match_prefix(raw) == DFAMatch{byte_count, {}});
    const SIMDMatcher nonaccepting(minimize(determinize(compile_regex("x|x[^!]*!"))));
    CHECK(nonaccepting.match_prefix(std::string(48, 'x')) == DFAMatch{1, {}});
    std::vector<DFAState> states(3);
    states[0].transitions['x'] = 1;
    states[0].transitions['y'] = 2;
    states[1].acceptance = Acceptance{3, 1, false};
    states[1].transitions['!'] = 2;
    states[2].transitions.fill(2); // A non-accepting all-byte trap.
    const SIMDMatcher trap(DFA(std::move(states)));
    CHECK(trap.match_prefix("x!" + raw) == DFAMatch{1, {3, 1, false}});
    CHECK(!trap.match_prefix("y" + raw));
    throws<std::invalid_argument>([&] { SIMDLexer bad(matcher.dfa()); });
}

struct GroupDigits : std::numpunct<char> {
    std::string do_grouping() const override { return "\3"; }
};

void codegen() {
    auto lexer = make_lexer({{"'; // never_emit_this_as_source\n", 123456}, {"a", 0}, {" +", 1, true}});
    auto c = emitC99(lexer, "the_scanner");
    auto assembly = emitAssembly(lexer.dfa(), "the_scanner");
    CHECK(c.find("int the_scanner(") != std::string::npos);
    CHECK(assembly.find(".globl the_scanner\n") != std::string::npos);
    CHECK(c.find("byte_class[input[i]]") != std::string::npos);
    CHECK(assembly.find(".byte ") != std::string::npos);
    CHECK(emitC99(full_byte_alphabet()).find("transitions[][256]") != std::string::npos);
    CHECK(emitAssembly(full_byte_alphabet()).find(".byte ") == std::string::npos);
    const auto marginal = full_byte_alphabet(true);
    CHECK(marginal.byte_classes().representatives.size() == byte_count - 1);
    CHECK(emitC99(marginal).find("transitions[][256]") != std::string::npos);
    CHECK(emitAssembly(marginal).find(".byte ") == std::string::npos);
    CHECK(c.find("never_emit_this_as_source") == std::string::npos);
    CHECK(assembly.find("never_emit_this_as_source") == std::string::npos);
    auto saved = std::locale::global(std::locale(std::locale::classic(), new GroupDigits));
    auto locale_c = emitC99(lexer.dfa(), "the_scanner");
    auto locale_assembly = emitAssembly(lexer, "the_scanner");
    std::locale::global(saved);
    CHECK(c == locale_c && assembly == locale_assembly);
    for (auto name : {"", "9scanner", "_reserved", "bad\n.globl other", "bad*/", "int", "size_t", "CHAR_BIT",
                      "this_scanner_name_is_over_thirty_one_characters"}) {
        throws<std::invalid_argument>([&] { emitC99(lexer, name); });
        throws<std::invalid_argument>([&] { emitAssembly(lexer, name); });
    }
    const auto empty_match = determinize(compile_regex("z?"));
    throws<std::invalid_argument>([&] { emitC99(empty_match); });
    throws<std::invalid_argument>([&] { emitAssembly(empty_match); });
}

} // namespace

int main() {
    try {
        regex_and_nfa();
        subset_budget();
        byte_classes();
        minimization();
        lexing();
        simd();
        codegen();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
