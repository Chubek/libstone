#include "LibDSL.hpp"

#include <iostream>

int main(int argc, char **argv)
try
{
  using namespace dsl;
  auto cli = cli::OptParser::from_text(R"(
    (program archive
      (description "Show a nested command and clustered Unix options.")
      (flag verbose (short v) (repeatable true) (description "Increase verbosity"))
      (option render (value true) (choices plain color grayscale) (default plain))
      (command files (require-subcommand true)
        (command run (description "Show which archive and member names were selected")
          (flag extract (short x) (requires file) (description "Select extract mode"))
          (flag list (short l) (requires file) (description "Select listing mode"))
          (option file (short f) (value true) (description "Archive filename"))
          (group mode (members extract list) (count 1 1))
          (positional member (required false) (repeatable true)))))
  )");
  const auto parsed = cli.parse(std::vector<std::string>(argv + 1, argv + argc));
  if (!parsed)
    {
      for (const auto &error : parsed.errors) std::cerr << error << '\n';
      return 2;
    }
  const auto rendering = parsed.value("render").value_or("plain");
  auto mode = terminal::RenderMode::plain;
  if (rendering == "color") mode = terminal::RenderMode::true_color;
  else if (rendering == "grayscale") mode = terminal::RenderMode::grayscale;
  if (parsed.help_requested || parsed.command_path.size() != 2)
    {
      std::cout << cli.help(parsed.command_path, mode);
      return 0;
    }
  terminal::StyledText out;
  out.text(parsed.has("extract") ? "extract " : "list ");
  out.text(parsed.value("file").value());
  for (const auto &member : parsed.positionals) out.text(" ").text(member);
  out.line_break();
  std::cout << out.render(mode);
  return 0;
}
catch (const std::exception &error)
{
  std::cerr << dsl::terminal::sanitize_text(error.what()) << '\n';
  return 1;
}
