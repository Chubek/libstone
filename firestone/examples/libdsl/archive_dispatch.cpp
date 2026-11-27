#include "LibDSL.hpp"

#include <iostream>

// Try: archive files -xvf archive.tar
// Or:  archive files --help
int main(int argc, char **argv)
{
  const auto cli = dsl::cli::OptParser::from_text(R"(
    (program archive
      (description "Demonstrate a hierarchical archive CLI without opening any files.")
      (require-subcommand true)
      (flag verbose (short v) (repeatable true))
      (command files
        (description "Show the requested archive operation and its arguments.")
        (handler print_request)
        (flag extract (short x) (requires file))
        (flag list (short l))
        (group operation (members extract list) (count 1 1))
        (option file (short f) (value true) (description "Archive filename"))
        (option level (value true) (type integer) (range 0 9) (default 2))
        (positional member (required false) (repeatable true)))))");

  const dsl::cli::Handlers handlers{
    {"print_request", [](const auto &result)
      {
        const auto safe = [](std::string_view value) { return dsl::terminal::sanitize_text(value); };
        std::cout << (result.has("extract") ? "extract" : "list");
        if (const auto file = result.value("file"))
          std::cout << ' ' << safe(*file);
        std::cout << " (level " << result.value("level").value_or("?") << ")\n";
        for (const auto &member : result.positionals)
          std::cout << "  " << safe(member) << '\n';
      }}
  };

  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i)
    args.emplace_back(argv[i]);
  const auto result = cli.dispatch(args, handlers);
  for (const auto &error : result.diagnostics)
    std::cerr << error.message << '\n';
  if (!result || result.help_requested)
    std::cout << cli.help(result.command_path);
  return result ? 0 : 2;
}
