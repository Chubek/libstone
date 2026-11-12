// The DSL wrapper in C++20: the original guide's chapter 12 examples plus
// patterns, literals, expression templates, parsers, PEG and task chains.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include <memtkx/MemTKXDSL.hpp>

namespace dsl = MemTKX::dsl;

struct Numbers : dsl::DSL<Numbers, dsl::Pipeline, dsl::Operators> {};

enum class State { Unmarked, Marked, Other };

struct Scalar : dsl::DSL<Scalar, dsl::ExprTemplates> {
  double val;
  explicit Scalar(double v) : val(v) {}
  double expr_value() const { return val; }
};

struct UnitsDSL : dsl::DSL<UnitsDSL, dsl::CustomLiterals> {
  static constexpr auto literals = dsl::literal_set(
      dsl::lit<"_km">([](long double v) { return v * 1000.0L; }),
      dsl::lit<"_m">([](long double v) { return v; }),
      dsl::lit<"_cm">([](long double v) { return v / 100.0L; }));
};

struct QueryDSL : dsl::DSL<QueryDSL, dsl::AST, dsl::Rewrite> {
  static inline auto rules = dsl::rewrite_set(dsl::rule<"double-neg">(
      [](const dsl::ASTNode& n) {
        return n.tag() == "not" && n.size() == 1 && n.child(0).tag() == "not" &&
               n.child(0).size() == 1;
      },
      [](const dsl::ASTNode& n) { return n.child(0).child(0); }));
};

static_assert(dsl::Pipeable<Numbers>);
static_assert(dsl::ExprLike<Scalar>);
static_assert(dsl::HasLiterals<UnitsDSL>);
static_assert(dsl::Rewritable<QueryDSL>);
static_assert(dsl::feature_mask<dsl::Pipeline, dsl::AST> ==
              (MTKX_FEATURE_PIPELINE | MTKX_FEATURE_AST));

int main() {
  // 12.2 pipelines and predicates
  {
    Numbers numbers;
    auto result = numbers.wrap(10) | dsl::pipe([](int value) { return value * 2; }) |
                  dsl::pipe([](int value) { return value + 1; });
    assert(result == 21);
    auto positive = dsl::predicate([](int value) { return value > 0; });
    auto even = dsl::predicate([](int value) { return value % 2 == 0; });
    auto accepted = positive & even;
    assert(accepted(4) && !accepted(-2) && !accepted(3));
    auto small = Numbers::make_pred([](int v) { return v < 100; });
    assert((positive & small)(42) && !(positive & small)(200));
  }

  // 12.3 dispatch tables, value and pattern clauses
  {
    auto transition = dsl::match(
        dsl::when<State::Unmarked>([](State, int) { return State::Marked; }),
        dsl::when<State::Marked>([](State, int) { return State::Marked; }),
        dsl::otherwise([](State state, int) { return state; }));
    assert(transition(State::Unmarked, 0) == State::Marked);
    assert(transition(State::Other, 0) == State::Other);

    auto classify = dsl::match(
        dsl::when<dsl::pattern<"[0-9]+">{}>([](std::string_view) { return std::string("number"); }),
        dsl::when<dsl::pattern<"[a-z][a-z0-9]*">{}>([](std::string_view) {
          return std::string("ident");
        }),
        dsl::otherwise([](std::string_view) { return std::string("other"); }));
    assert(classify(std::string_view("123")) == "number");
    assert(classify(std::string_view("x9")) == "ident");
    assert(classify(std::string_view("!")) == "other");

    auto strict = dsl::match(dsl::when<1>([](int) { return 1; }));
    bool threw = false;
    try {
      (void)strict(2);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw);
    assert(dsl::Pattern("a+b").matches("aaab") && !dsl::Pattern("a+b").matches("b"));
  }

  // 12.4 AST and rewrite rules
  {
    auto tree = dsl::node<"identity">(dsl::leaf<"number">(42));
    auto rules = dsl::rewrite_set(dsl::rule<"remove-identity">(
        [](const dsl::ASTNode& node) {
          return node.tag() == "identity" && node.children().size() == 1;
        },
        [](const dsl::ASTNode& node) { return node.child(0); }));
    auto simplified = rules.apply(tree);
    assert(simplified.dump() == "(number 42)");
    assert(tree.dump() == "(identity (number 42))");

    auto expr = dsl::node<"add">(dsl::leaf<"num">(1), dsl::leaf<"num">(2.5));
    assert(expr.dump() == "(add (num 1) (num 2.5))");
    QueryDSL q;
    auto nested = q.make_node<"not">(q.make_node<"not">(dsl::leaf("var", "x")));
    assert(q.rewrite(nested).dump() == "(var x)");
    auto select = dsl::node("select", dsl::leaf("col", "name"), dsl::leaf("col", "age"));
    assert(select.dump() == "(select (col name) (col age))");
    assert(dsl::ASTNode().dump() == "()");

    auto throwing = dsl::rewrite_set(dsl::rule(
        "boom", [](const dsl::ASTNode&) -> bool { throw std::domain_error("pred"); },
        [](const dsl::ASTNode& n) { return n; }));
    bool threw = false;
    try {
      (void)throwing.apply(tree);
    } catch (const std::domain_error&) {
      threw = true;
    }
    assert(threw);
  }

  // 12.5 results, maybe, lazy, memoize
  {
    int computations = 0;
    auto square = dsl::memoize<int, int>([&](int value) {
      ++computations;
      return value * value;
    });
    assert(square(7) == 49);
    assert(square(7) == 49 && computations == 1);
    square.clear_cache();
    assert(square(7) == 49 && computations == 2);

    dsl::Lazy<int> deferred([] { return 42; });
    assert(!deferred.is_computed());
    assert(deferred.get() == 42 && deferred.is_computed());
    assert(dsl::Maybe<int>(5).map([](int value) { return value + 1; }).or_else(0) == 6);
    std::optional<int> v = 3;
    assert(*dsl::map(v, [](int x) { return x * 2; }) == 6);

    auto r = dsl::Result<int, std::string>::from_ok(4).map([](int x) { return x + 1; });
    assert(r.unwrap() == 5);
    auto chained = r.and_then([](int x) {
      return x > 10 ? dsl::Result<int, std::string>::from_ok(x)
                    : dsl::Result<int, std::string>::from_err("too small");
    });
    assert(chained.is_err() && chained.unwrap_err() == "too small");
  }

  // expression templates and literals
  {
    Scalar a{3.0}, b{4.0};
    auto expr = a + b * a;
    assert(expr.eval() == 15.0);
    auto neg = -a;
    assert(neg.eval() == -3.0);
    assert(UnitsDSL::parse_literal("3.5_km") == 3500.0L);
    assert(UnitsDSL::parse_literal("250_cm") == 2.5L);
    bool threw = false;
    try {
      (void)UnitsDSL::parse_literal("5_mi");
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw);
  }

  // 12.6 parser combinators
  {
    auto pair = dsl::ch('a') & dsl::ch('b');
    auto good = dsl::run_parser(pair, "ab");
    assert(good.value.has_value());
    assert(good.value->first == 'a' && good.value->second == 'b');

    auto trailing = dsl::run_parser(pair, "abc");
    assert(!trailing.value.has_value());
    assert(trailing.error.pos == 2);

    auto alternative = dsl::try_parse(pair) | (dsl::ch('a') & dsl::ch('c'));
    assert(dsl::run_parser(alternative, "ac").value.has_value());

    auto digit = dsl::satisfy([](char c) { return c >= '0' && c <= '9'; }, "digit");
    auto digits = *digit;
    auto parsed = dsl::run_parser(digits, "2024");
    assert(parsed.value && parsed.value->size() == 4 && (*parsed.value)[3] == '4');

    // a custom parser producing a non-native value type (std::string)
    auto word = dsl::parser([](dsl::ParsecInput& in) -> dsl::ExpectedResult<std::string> {
      const auto begin = in.pos;
      while (!in.eof() && in.peek() >= 'a' && in.peek() <= 'z') {
        in.consume();
      }
      if (in.pos == begin) {
        return dsl::fail_expected<std::string>(in, "word");
      }
      return std::string(in.get_span(begin));
    });
    auto sentence = word & dsl::optional(dsl::ch('!'));
    auto s = dsl::run_parser(sentence, "hello!");
    assert(s.value && s.value->first == "hello" && s.value->second == '!');
    auto miss = dsl::run_parser(word, "123");
    assert(!miss.value && miss.error.expected == std::vector<std::string>{"word"});
    auto labeled = dsl::labeled(dsl::ch('q'), "letter q");
    assert(dsl::run_parser(labeled, "x").error.expected.front() == "letter q");

    dsl::ParsecInput in{"xyz", 0};
    auto x = dsl::ch('x')(in);
    assert(x && *x == 'x' && in.pos == 1);

    std::unordered_map<std::string, std::string> semantics{{"number", "decimal"}};
    dsl::ParsecInput num_in{"42rest", 0};
    auto span = dsl::parse_with_action(digits, num_in, semantics,
                                       [](const auto& sem, dsl::ParserStage stage) {
                                         return std::string(stage.span) + ":" + sem.at("number");
                                       });
    assert(span && *span == "42:decimal");

    dsl::ParseError merged{3, dsl::ParseFailureKind::Soft, {"a"}};
    dsl::merge_error(merged, dsl::ParseError{3, dsl::ParseFailureKind::Committed, {"b"}});
    assert(merged.kind == dsl::ParseFailureKind::Committed && merged.expected.size() == 2);

    auto exploding = dsl::parser([](dsl::ParsecInput&) -> dsl::ExpectedResult<char> {
      throw std::length_error("parser exploded");
    });
    bool threw = false;
    try {
      (void)dsl::run_parser(dsl::ch('a') & exploding, "ab");
    } catch (const std::length_error&) {
      threw = true;
    }
    assert(threw);

    auto loaded = dsl::load_parse_input_file("/definitely/missing/file");
    assert(loaded.is_err());
    auto prod = dsl::production<"number">([](int v) { return v; });
    assert(decltype(prod)::id == "number" && prod.fn(5) == 5);
  }

  // PEG definitions, channels, matcher and combinators
  {
    std::vector<std::string> idents;
    auto peg = dsl::create_peg_definition();
    auto& ident = peg.add_rule<"[a-z][a-z0-9]*">(
        [&](dsl::PEGMatch& m) { idents.emplace_back(m.value); });
    auto& number = peg.add_rule<"[0-9]+">();
    auto& space = peg.add_rule<"[ \n]+">();
    space.channel = dsl::PEGIgnoreChannel;
    auto result = peg.parse("ab 12 c3");
    assert(result.ok() && idents == (std::vector<std::string>{"ab", "c3"}));
    auto bad = peg.parse("ab #");
    assert(bad.failed() && bad.error_offset() == 3 && bad.error_column() == 4);

    bool threw = false;
    try {
      peg.add_rule("[unterminated");
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    assert(threw);
    threw = false;
    try {
      (void)dsl::new_peg_channel("NOAT");
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    assert(threw);

    auto child = dsl::derive_peg_definition(peg);
    int hashes = 0;
    child.add_rule("#", [&](dsl::PEGMatch&) { ++hashes; });
    assert(child.rules().size() == 4 && child.rules().front() == &ident);
    assert(child.parse("ab #").ok() && hashes == 1);

    // the matcher
    std::vector<std::string> seen;
    int wild = 0;
    auto matcher = dsl::peg_new_matcher(peg, "ab 12 ?x");
    while (!matcher.is_spent()) {
      matcher << ident([&](dsl::PEGMatch& m) { seen.emplace_back(m.value); })
              << number([&](dsl::PEGMatch& m) {
                   assert(m.rule == &number);
                   seen.emplace_back(m.value);
                 })
              << matcher.wildcard([&](dsl::PEGMatch&) { ++wild; });
    }
    matcher.close();
    assert((seen == std::vector<std::string>{"ab", "12", "x"}) && wild == 1);

    number.channel = dsl::new_peg_channel("@NUM");
    auto filtered = dsl::peg_new_matcher(peg, "12");
    filtered.only("@DEFAULT");
    assert(!filtered.channel_allowed("@NUM"));
    filtered << number([&](dsl::PEGMatch&) { assert(false); })
             << filtered.wildcard([&](dsl::PEGMatch&) { ++wild; });
    assert(wild == 2 && filtered.position() == 1);

    // combinators
    auto w = dsl::peg_rule(ident);
    auto n = dsl::peg_rule(number);
    auto ws = dsl::peg_rule(space);
    auto seq = dsl::peg_seq(w, ws, n);
    auto r1 = dsl::run_parser(seq, "ab 12");
    assert(r1.value && r1.value->value == "ab 12" && r1.value->begin == 0);
    auto choice = dsl::peg_choice(n, w);
    auto r2 = dsl::run_parser(choice, "77");
    assert(r2.value && r2.value->rule == &number && r2.value->channel.name == "@NUM");
    auto many = dsl::peg_many(dsl::peg_seq(w, dsl::peg_opt(ws)));
    assert(dsl::run_parser(many, "a b c").value->size() == 3);
    auto many1 = dsl::peg_many1(n);
    assert(!dsl::run_parser(many1, "").value);
    auto look = dsl::peg_seq(dsl::peg_and(w), w);
    assert(dsl::run_parser(look, "abc").value);
    auto neg = dsl::peg_seq(dsl::peg_not(n), w);
    assert(dsl::run_parser(neg, "abc").value && !dsl::run_parser(neg, "12").value);
    auto tagged = dsl::peg_channel("@WORD", w);
    auto r3 = dsl::run_parser(tagged, "xy");
    assert(r3.value && r3.value->channel.name == "@WORD" && !r3.value->ignored);
    auto span = dsl::peg_detail::as_span_parser(dsl::ch('a') & dsl::ch('b'));
    assert(dsl::run_parser(span, "ab").value->length() == 2);
  }

  // DSL task chains
  {
    dsl::Task load{"load", [](dsl::TaskState&) {
                     return dsl::Result<std::string, std::string>::from_ok("ok");
                   }};
    dsl::Task check{"check", [](dsl::TaskState& st) {
                      return st.results.count("load") != 0
                                 ? dsl::Result<std::string, std::string>::from_ok("saw load")
                                 : dsl::Result<std::string, std::string>::from_err("missing");
                    }};
    dsl::Task fail{"fail", [](dsl::TaskState&) {
                     return dsl::Result<std::string, std::string>::from_err("broken");
                   }};
    dsl::Task after{"after", [](dsl::TaskState&) {
                      return dsl::Result<std::string, std::string>::from_ok("after");
                    }};
    auto chain = load | check | fail | after;
    dsl::TaskState state;
    dsl::run(state, chain);
    assert(state.results.size() == 3);
    assert(state.results.at("check").unwrap() == "saw load");
    assert(state.results.at("fail").is_err());

    chain.policy = dsl::TaskPolicy::ContinueOnError;
    dsl::run(state, chain);
    assert(state.results.size() == 4);

    dsl::Task pause{"pause", [](dsl::TaskState& st) {
                      dsl::suspend(st);
                      return dsl::Result<std::string, std::string>::from_ok("paused");
                    }};
    dsl::TaskState paused;
    dsl::run(paused, pause | after);
    assert(paused.suspended && paused.results.size() == 1);
    dsl::upon(paused, [] { return true; }, after);
    assert(paused.results.count("after") == 1);
  }
  return 0;
}
