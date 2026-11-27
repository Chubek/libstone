#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include "LibDSL.hpp"

using namespace dsl;
using namespace dsl::repl;

int main()
{
  const auto analysis = Analysis::parse(
      R"((analysis (delimiters "()[]") (block begin end) (indent-width 3)
                   (keywords let lambda) (symbols late last) (directives load list)))");
  const std::string_view source = "begin\n  (let \"[escaped \\\" end\" [12]) ; ])\nend";
  auto open = analysis.inspect(source, 0);
  assert(open.syntax == live::SyntaxStatus::complete);
  assert(open.delimiters.size() == 6);
  assert(open.match() && open.match()->begin == source.rfind("end"));
  assert(open.delimiters[2].depth == 2);
  assert(open.navigate(0, true)->begin == source.find('('));
  assert(analysis.inspect(source, source.find("let")).indentation == 3);
  assert(analysis.inspect(source, source.find("let")).next_indentation == 6);
  assert(analysis.inspect(source, source.size()).indentation == 0);
  assert(analysis.inspect("(\"unterminated", 3).syntax == live::SyntaxStatus::incomplete);
  assert(analysis.inspect("(; ignored ]", 3).syntax == live::SyntaxStatus::incomplete);
  auto mismatch = analysis.inspect("([)]", 2);
  assert(mismatch.syntax == live::SyntaxStatus::invalid);
  assert(mismatch.diagnostics.size() == 4);
  assert(!mismatch.match());

  auto symbols = analysis.complete("(lambxx)", 4);
  assert(symbols.context.kind == CompletionKind::symbol);
  assert((symbols.context.replacement == live::SourceSpan{1, 7}));
  assert(symbols.context.prefix == "lam");
  assert((symbols.candidates == std::vector<std::string>{"lambda"}));
  assert(analysis.complete("(; last", 6).context.kind == CompletionKind::suppressed);
  assert(analysis.complete("(\"last\")", 5).context.kind == CompletionKind::suppressed);
  auto directive = analysis.complete(" :lox", 4);
  assert(directive.context.kind == CompletionKind::directive);
  assert((directive.candidates == std::vector<std::string>{"load"}));

  auto custom = analysis;
  custom.add_completion_provider(CompletionSource::semantic, [](const auto& context, const auto& add)
  {
    if (context.kind == CompletionKind::symbol)
      {
        add("last", "Preferred", 50);
        add("late\ninjected", {}, 70);
      }
    if (context.kind == CompletionKind::argument && context.directive == "load" && context.argument == 1)
      add("next", "Second argument", 5);
  });
  auto ranked = custom.complete("la", 2);
  assert(ranked.candidates.front() == "last" && ranked.details.front().description == "Preferred");
  assert(ranked.candidates.size() == 3);
  assert((custom.complete(":load \"two words\" ne", 20).candidates == std::vector<std::string>{"next"}));

  int effects = 0;
  cli::Option mode;
  mode.name = "mode";
  mode.takes_value = true;
  mode.choices = {"quick", "quiet"};
  mode.predicate = [&](const auto&) { ++effects; return true; };
  cli::Command command;
  command.name = "load";
  command.options.push_back(mode);
  command.handler = [&](const auto&) { ++effects; };
  custom.add_option_completion("load", std::make_shared<const cli::OptParser>(command));
  std::string_view argument = ":load --mode qu";
  auto options = custom.complete(argument, argument.size());
  assert((options.candidates == std::vector<std::string>{"quick", "quiet"}));
  assert(effects == 0);

  // The PEG regex spelling and channels are used as lexical metadata.
  auto lexical = Analysis::parse(R"((analysis (channel @WORDS keyword) (pattern "+" number)))");
  auto words = std::make_shared<PEGDefinition>();
  words->add_rule<"[a-z]+">([&](const PEGMatch&) { ++effects; }).channel = new_peg_channel("@WORDS");
  words->add_rule<"+">([&](const PEGMatch&) { ++effects; });
  lexical.set_peg(words);
  const auto colored = lexical.inspect("red+blue", 4);
  assert(colored.syntax == live::SyntaxStatus::complete);
  assert(colored.highlights.size() == 3);
  assert(colored.highlights[0].kind == HighlightKind::keyword);
  assert(colored.highlights[1].kind == HighlightKind::number);
  const auto cfg = std::make_shared<const live::SyntaxGrammar>(live::SyntaxGrammar{
    "expression",
    {{"add", "expression", {{live::SyntaxSymbolKind::literal, "red"},
                            {live::SyntaxSymbolKind::literal, "+"},
                            {live::SyntaxSymbolKind::literal, "blue"}}, ""}}
  });
  lexical.set_cfg(cfg);
  assert(lexical.inspect("red+blue", 4).syntax == live::SyntaxStatus::complete);
  assert(lexical.inspect("red+", 4).syntax == live::SyntaxStatus::incomplete);
  lexical.set_grammar_provider([](std::string_view text, std::size_t cursor, std::stop_token) {
    live::CursorAnalysis out;
    out.status = live::SyntaxStatus::incomplete;
    out.replacement = {0, text.size()};
    out.cursor_expected = {cursor ? "red" : "blue"};
    return out;
  });
  assert(lexical.inspect("r", 1).syntax == live::SyntaxStatus::incomplete);
  assert((lexical.complete("r", 1).candidates == std::vector<std::string>{"red"}));
  auto callbacks = analysis_callbacks(std::make_shared<const Analysis>(lexical));
  assert(callbacks.continuation("r") == live::SyntaxStatus::incomplete);
  assert(callbacks.complete("r", 1).front().text == "red");
  assert(effects == 0);

  AnalysisRevision revisions;
  auto old = revisions.replace();
  auto current = revisions.replace();
  assert(!revisions.current(old) && revisions.current(current) && old.stop_requested());
  assert(custom.inspect(source, 0, old).cancelled);
  assert(custom.complete("la", 2, old).candidates.empty());
  assert(!custom.inspect(source, 0, current).cancelled);

  auto styled = highlighted(source, open);
  for (auto mode : {terminal::RenderMode::true_color, terminal::RenderMode::grayscale,
                    terminal::RenderMode::monochrome})
    assert(!styled.render(mode).empty());
  assert(styled.render(terminal::RenderMode::plain) == source);
  std::string_view unsafe = "test\x1b[2J";
  const auto safe = highlighted(unsafe, analysis.inspect(unsafe, unsafe.size()));
  assert(safe.render(terminal::RenderMode::plain).find('\x1b') == std::string::npos);
  const auto shared = std::make_shared<const Analysis>(analysis);
  auto renderer = analysis_renderer(shared);
  EditView view;
  view.document = unsafe;
  view.visible_end = view.cursor_byte = unsafe.size();
  assert(renderer(view).render() == terminal::sanitize_text(unsafe));
  Editor editor;
  assert(editor.insert("("));
  assert(analysis_indenter(shared)(editor, Command::newline, {}));
  assert(editor.text() == "(\n   ");
  open.highlights.front().span.end = source.size() + 1;
  try
    {
      (void)highlighted(source, open);
      assert(false);
    }
  catch (const std::invalid_argument&) {}

  const auto aligned = Analysis::parse("(analysis (align-arguments true))");
  assert(aligned.inspect("  (value\nx", 10).indentation == 3);
  const auto tiny = [] {
    AnalysisConfig cfg;
    cfg.max_depth = 1;
    return Analysis(cfg);
  }();
  try
    {
      (void)tiny.inspect("((", 0);
      assert(false);
    }
  catch (const std::length_error&) {}
}
