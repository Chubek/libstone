#include "LibDSL.hpp"

#include <iostream>

namespace {
struct StreamBackend
{
  std::size_t read(std::span<std::byte> output)
  {
    if (output.empty()) return 0;
    auto ch = std::cin.get();
    if (ch == std::char_traits<char>::eof()) return 0;
    output.front() = static_cast<std::byte>(static_cast<unsigned char>(ch));
    return 1;
  }

  std::size_t write(std::span<const std::byte> input)
  {
    std::cout.write(reinterpret_cast<const char *>(input.data()),
                    static_cast<std::streamsize>(input.size()));
    std::cout.flush();
    return std::cout ? input.size() : 0;
  }
};
}

int main(int argc, char **argv)
try
{
  using namespace dsl;
  auto command = cli::OptParser::from_text(R"(
    (command live-lisp (description "A small closure-capable synthesized language")
      (option script (description "Absolute path to a trusted :script executable")
                     (value true)))
  )");
  const std::vector<std::string> arguments(argv + 1, argv + argc);
  const auto options = command.parse(arguments);
  if (!options)
    {
      for (const auto &error : options.errors) std::cerr << error << '\n';
      return 2;
    }
  if (options.help_requested)
    {
      std::cout << command.help();
      return 0;
    }

  const synth::MetaLanguage language(synth::Specification::from_text(R"(
    (language MiniLisp
      (macro (unless $test $otherwise $then) (if $test $then $otherwise)))
  )"));
  synth::MetaEvaluator evaluator;
  auto directives = std::make_shared<repl::Directives>(
    repl::Directives::from_text(R"(
      (command directives
        (command about (description "Show available language forms"))
        (command script (description "Run the optionally installed example script")))
    )"));
  directives->bind({"about"}, [](const cli::ParseResult &) {
    return repl::Evaluation{true, "Use lambda, let, if, quote, list, or unless. Try : for help."};
  });
  if (auto path = options.value("script"))
    directives->bind_script({"script"}, repl::scripts::Runner::trusted({std::string(*path)}));

  repl::AnalysisConfig config;
  config.keywords = {"lambda", "let", "if", "quote", "unless"};
  config.symbols = {"list"};
  config.directives = {"about", "script"};
  config.align_arguments = true;
  auto analysis = std::make_shared<repl::Analysis>(config);
  auto callbacks = repl::analysis_callbacks(analysis);
  callbacks.continuation = [&](std::string_view source) {
    if (repl::Directives::recognizes(source)) return live::SyntaxStatus::complete;
    return analysis->inspect(source, source.size()).syntax;
  };
  callbacks.indentation = [&](std::string_view source, std::size_t cursor) {
    return analysis->inspect(source, cursor).next_indentation;
  };

  repl::Session session(std::move(callbacks), repl::Directives::route(
    directives, [&](std::string_view source) -> repl::Evaluation {
      try
        {
          const auto value = evaluator.evaluate(language.expand(source));
          if (!value.is_syntax()) return {true, "<callable>"};
          return {true, value.as_syntax().dump()};
        }
      catch (const std::exception &error)
        {
          return {false, error.what()};
        }
    }));

  StreamBackend backend;
  IO io(backend);
  repl::RunOptions settings;
  session.set_auto_indent(true);
  session.set_prompt([](std::string_view, std::size_t, bool continuing) {
    terminal::Style style;
    style.bold = true;
    style.foreground = terminal::Color{85, 180, 230};
    terminal::StyledText prompt;
    prompt.span(continuing ? "....> " : "lisp> ", style);
    return std::vector{std::move(prompt)};
  });
  session.set_renderer([&](const repl::EditView &view) {
    repl::AnalysisResult visible;
    for (auto highlight : analysis->inspect(view.document, view.cursor_byte).highlights)
      {
        auto begin = std::max(highlight.span.begin, view.visible_begin);
        auto end = std::min(highlight.span.end, view.visible_end);
        if (begin < end)
          {
            highlight.span = {begin - view.visible_begin, end - view.visible_begin};
            visible.highlights.push_back(highlight);
          }
      }
    const auto source = view.document.substr(view.visible_begin,
                                             view.visible_end - view.visible_begin);
    auto result = repl::highlighted(source, visible);
    // Control bytes may have been hidden by the editor before measurement.
    if (result.render() != view.text)
      {
        terminal::StyledText fallback;
        fallback.text(view.text);
        return fallback;
      }
    return result;
  });
  repl::RunStatus status;
#if LIBDSL_LIVE_POSIX
  const auto *term = std::getenv("TERM");
  const auto backend_caps = terminal::discover_capabilities(
    terminal::descriptor_backend(STDIN_FILENO, STDOUT_FILENO));
  if (backend_caps.interactive && term && std::string_view(term) != "dumb")
    {
      if (backend_caps.true_color()) settings.color = terminal::RenderMode::true_color;
      else if (backend_caps.colors() >= 256) settings.color = terminal::RenderMode::color256;
      else if (backend_caps.colors() >= 8) settings.color = terminal::RenderMode::ansi;
      status = session.run_terminal(STDIN_FILENO, STDOUT_FILENO, settings);
    }
  else
#endif
    status = session.run_lines(io);
  return status == repl::RunStatus::eof || status == repl::RunStatus::stopped ? 0 : 2;
}
catch (const std::exception &error)
{
  std::cerr << dsl::terminal::sanitize_text(error.what()) << '\n';
  return 1;
}
