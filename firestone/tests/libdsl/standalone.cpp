#include "LibDSL.hpp"
#include <cassert>

dsl::SExpr from_peer(std::string_view source);
std::string render_from_peer(std::string_view source);

struct Composite : dsl::DSL<Composite, dsl::GlobalEscapeCodes, dsl::Terminfo,
                            dsl::OptParser, dsl::REPL, dsl::LanguageSynthesizer>
{
};

static_assert(std::is_same_v<decltype(&dsl::parse_sexpr),
                            dsl::SExpr (*)(std::string_view)>);

int main()
{
  const auto source = "(root (x) \"y\")";
  auto parsed = dsl::parse_bounded_sexpr(source);
  assert(parsed == from_peer(source));
  assert(parsed.dump() == source);
  auto native = Composite::styled_text();
  native.text("hello");
  assert(native.render() == render_from_peer("hello"));
  Composite::InputDecoder decoder;
  assert(decoder.feed("a").front().text == "a");
  const auto parser = Composite::OptionParser::from_text("(program sample)");
  assert(parser.parse({}));
  auto language = Composite::synthesize("(language Empty)");
  assert(language.expand("(quote x)").dump() == "(quote x)");
  auto session = Composite::repl_session();
  assert(session.syntax_status() == dsl::live::SyntaxStatus::complete);
  for (auto limits : {dsl::SExprLimits{0, 10, 10}, dsl::SExprLimits{100, 2, 10},
                      dsl::SExprLimits{100, 10, 0}})
    {
      bool rejected = false;
      try { (void)dsl::parse_bounded_sexpr(source, limits); }
      catch (const std::invalid_argument &) { rejected = true; }
      assert(rejected);
    }
}
