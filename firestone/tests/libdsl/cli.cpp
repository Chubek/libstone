#include "LibDSL.hpp"

#include <cstdlib>
#include <iostream>

namespace
{
using namespace dsl::cli;

void check(bool condition, const char *message)
{
  if (!condition)
    {
      std::cerr << "W06: " << message << '\n';
      std::exit(1);
    }
}

bool has_error(const ParseResult &result, ErrorCode code,
               std::optional<std::size_t> index = std::nullopt)
{
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const ParseError &error)
                     {
                       return error.code == code && (!index || error.argv_index == index);
                     });
}

Command native()
{
  Command root;
  root.name = "archive";
  root.description = "Archive tools";
  root.require_subcommand = true;
  Option verbose;
  verbose.name = "verbose";
  verbose.short_name = 'v';
  verbose.repeatable = true;
  root.options.push_back(verbose);

  Command files;
  files.name = "files";
  files.require_subcommand = true;
  Command run;
  run.name = "run";
  run.aliases = {"r"};
  run.description = "Ingest words and \x1b[31m 日本語日本語日本語, then process the archive.";
  run.handler_name = "run_archive";
  Option extract;
  extract.name = "extract";
  extract.short_name = 'x';
  extract.dependencies = {"file"};
  Option list;
  list.name = "list";
  list.short_name = 'l';
  Option file;
  file.name = "file";
  file.short_name = 'f';
  file.short_aliases = {'F'};
  file.aliases = {"archive-file"};
  file.takes_value = true;
  Option level;
  level.name = "level";
  level.kind = ValueKind::integer;
  level.takes_value = true;
  level.environment = "ARCHIVE_LEVEL";
  level.default_value = "2";
  level.range = NumericRange{"-10", "15"};
  Option format;
  format.name = "format";
  format.choices = {"tar", "zip"};
  format.takes_value = true;
  Option compress;
  compress.name = "compress";
  compress.negatable = true;
  compress.repeatable = true;
  compress.count.maximum = 2;
  compress.default_value = "true";
  run.options = {extract, list, file, level, format, compress};
  run.groups.push_back({"mode", {"extract", "list"}, {1, 1}});
  Positional member;
  member.name = "member";
  member.required = false;
  member.repeatable = true;
  member.count.maximum = 2;
  run.positionals.push_back(member);
  files.subcommands.push_back(run);
  root.subcommands.push_back(files);
  return root;
}

constexpr std::string_view source = R"(
(program archive
  (description "Archive tools")
  (require-subcommand true)
  (flag verbose (short v) (repeatable true))
  (command files
    (require-subcommand true)
    (command run
      (aliases r)
      (handler run_archive)
      (flag extract (short x) (requires file))
      (flag list (short l))
      (option file (short f) (short-aliases F) (aliases archive-file) (value true))
      (option level (value true) (type integer) (range -10 15)
                    (environment ARCHIVE_LEVEL) (default 2))
      (option format (value true) (choices tar zip))
      (switch compress (repeatable true) (count 0 2) (default true))
      (group mode (members extract list) (count 1 1))
      (positional member (required false) (repeatable true) (count 0 2)))))
)";

struct CLI : dsl::DSL<CLI, dsl::OptParser> {};
static_assert(std::is_same_v<CLI::OptionParser, OptParser>);

void parsing()
{
  const OptParser parser(native());
  const auto textual = OptParser::from_text(source);
  const Environment env = [](std::string_view key) -> std::optional<std::string>
  {
    return key == "ARCHIVE_LEVEL" ? std::optional<std::string>{"13"} : std::nullopt;
  };
  const Environment no_env = [](std::string_view) { return std::optional<std::string>{}; };
  const std::vector<std::string> argv = {
      "-v", "files", "r", "-xvf", "archive.tar", "--no-compress", "--level", "-7",
      "--format=tar", "--", "-v", "last"
  };
  auto result = parser.parse(argv, env);
  const auto other = textual.parse(argv, env);
  check(result && other && result.options == other.options
        && result.typed_options == other.typed_options
        && result.arguments == other.arguments && result.command_path == other.command_path,
        "native and S-expression parse equivalently");
  check(result.command_path == std::vector<std::string>{"files", "run"}
        && result.value("file") == "archive.tar" && result.occurrences.at("verbose") == 2
        && result.get<std::int64_t>("level") && *result.get<std::int64_t>("level") == -7
        && result.get<bool>("compress") && !*result.get<bool>("compress")
        && result.get_argument<std::string>("member")
        && *result.get_argument<std::string>("member") == "-v", "cluster and termination");

  result = parser.parse({"files", "run", "-xFarchive.tar"}, env);
  check(result && result.value("file") == "archive.tar"
        && result.get<std::int64_t>("level") && *result.get<std::int64_t>("level") == 13,
        "attached alias and environment default");
  result = parser.parse({"files", "run", "-x", "--archive-file=other.tar"}, no_env);
  check(result && result.value("file") == "other.tar" && result.value("level") == "2",
        "long alias, literal default");
  result = parser.parse({"files", "run", "-xvf", "--help"}, no_env);
  check(!result && result.help_requested && has_error(result, ErrorCode::missing_value, 2),
        "missing cluster operand preserves a following option");
  result = parser.parse({"files", "run", "-xfarchive", "--format=rar"}, no_env);
  check(!result && has_error(result, ErrorCode::invalid_value, 3), "invalid typed choice");
  result = parser.parse({"files", "run", "-l", "--level=16"}, no_env);
  check(!result && has_error(result, ErrorCode::invalid_value, 3), "numeric range predicate");
  result = parser.parse({"files", "run", "-x"}, no_env);
  check(!result && has_error(result, ErrorCode::dependency), "unsatisfied dependency");
  result = parser.parse({"files", "run", "-xlfarchive"}, no_env);
  check(!result && has_error(result, ErrorCode::group), "mutual exclusion");
  result = parser.parse({"files", "run"}, no_env);
  check(!result && has_error(result, ErrorCode::group), "required group");
  result = parser.parse({"files", "run", "-l", "--compress", "--no-compress", "--compress"});
  check(!result && has_error(result, ErrorCode::cardinality, 5), "option cardinality");
  result = parser.parse({"files", "run", "-l", "one", "two", "three"});
  check(!result && has_error(result, ErrorCode::unexpected_argument, 5), "variadic maximum");
  result = parser.parse({"files", "run", "-x", "-fa", "-Fb"});
  check(!result && has_error(result, ErrorCode::duplicate_option, 4), "duplicate through alias");
  result = parser.parse({"files", "typo"});
  check(!result && has_error(result, ErrorCode::unknown_command, 1), "unknown command vs operand");
  result = parser.parse({"files", "run", "--\x1b[2J"});
  check(!result && result.errors.front().find('\x1b') == std::string::npos, "safe diagnostics");

  int invoked = 0;
  const Handlers handlers{{"run_archive", [&](const ParseResult &) { ++invoked; }}};
  check(static_cast<bool>(textual.dispatch({"files", "r", "-l"}, handlers, no_env))
        && invoked == 1, "dispatch selected alias");
  check(static_cast<bool>(textual.dispatch({"files", "r", "--help"}, handlers, no_env))
        && !textual.dispatch({"files", "r"}, handlers, no_env) && invoked == 1,
        "no handler on help or error");
  check(has_error(textual.dispatch({"files", "r", "-l"}), ErrorCode::handler),
        "uninstalled handler has no implicit capability");
}

void validation()
{
  for (auto malformed : {
         "(program z (option a (short h)))",
         "(program z (option a) (command child (option a)))",
         "(program z (option no-a) (switch a))",
         "(program z (command child (aliases peer)) (command peer))",
         "(program z (positional tail (repeatable true)) (positional next))",
         "(program z (flag x (count 2 1)))",
         "(program z (group g (members x) (count 1 1)))",
         "(program z (option x (value true) (type integer) (default word)))",
         "(program z (option x (value true) (type integer) (range 1 -1)))",
         "(program z (option a (value true) (value false)))",
         "(program z (command child (dialect (cluster-short false))))",
         "(program z (switch a (unknown true)))",
         "(program z) (program second)",
         "(program z"})
    {
      bool rejected = false;
      try { (void)OptParser::from_text(malformed); }
      catch (const std::invalid_argument &) { rejected = true; }
      check(rejected, malformed);
    }

  auto model = native();
  model.subcommands.front().subcommands.front().options[2].short_name = 'v';
  bool rejected = false;
  try { (void)OptParser(model); }
  catch (const std::invalid_argument &) { rejected = true; }
  check(rejected, "native validation uses inherited namespace");

  auto constrained = OptParser::from_text(R"(
    (program test
      (dialect (cluster-short false) (attached-short false) (long-equals false)
               (options-after-positionals false) (negative-numbers false))
      (flag verbose (short v))
      (option file (short f) (value true))
      (positional argument (repeatable true))))");
  check(has_error(constrained.parse({"-vv", "input"}), ErrorCode::dialect, 0)
        && has_error(constrained.parse({"-fabc", "input"}), ErrorCode::dialect, 0)
        && has_error(constrained.parse({"--file=abc", "input"}), ErrorCode::dialect, 0)
        && has_error(constrained.parse({"-7"}), ErrorCode::unknown_option, 0),
        "configurable token dialect");
  auto result = constrained.parse({"input", "--file", "abc"});
  check(result && !result.has("file") && result.positionals.size() == 3,
        "dialect can stop option parsing at first positional");
  const auto ambiguous = OptParser::from_text("(program test (command file) (positional file))");
  result = ambiguous.parse({"file"});
  check(result && result.command_path == std::vector<std::string>{"file"},
        "a matching child wins over a positional of the same spelling");
  result = ambiguous.parse({"--", "file"});
  check(result && result.command_path.empty() && result.positionals == std::vector<std::string>{"file"},
        "terminator permits positionals named like subcommands");

  auto with_predicate = native();
  int predicate_calls = 0;
  with_predicate.subcommands.front().subcommands.front().options[3].predicate =
      [&](const Value &value) { ++predicate_calls; return std::get<std::int64_t>(value) >= 0; };
  const OptParser parser(with_predicate);
  (void)parser.complete({"files", "run", "-l", "--level", "-2"}, "--");
  check(predicate_calls == 0, "completion must not invoke predicates");
  result = parser.parse({"files", "run", "-l", "--level", "-2"});
  check(!result && predicate_calls == 1, "native predicate validates parsed option");
}

void help_and_completion()
{
  const OptParser parser(native());
  using dsl::terminal::RenderMode;
  auto plain = parser.help({"files", "run"}, RenderMode::plain, 33);
  check(plain.find("Usage: archive files run") != std::string::npos
        && plain.find("--verbose") != std::string::npos
        && plain.find('\x1b') == std::string::npos, "nested plain help");
  std::string_view lines(plain);
  while (!lines.empty())
    {
      const auto n = lines.find('\n');
      check(n != lines.npos && dsl::terminal::display_width(lines.substr(0, n)) <= 33,
            "help wraps Unicode display width");
      lines.remove_prefix(n + 1);
    }
  check(parser.help({"files", "run"}, RenderMode::true_color).find("\x1b[") != std::string::npos
        && parser.help({}, RenderMode::grayscale).find("\x1b[") != std::string::npos
        && parser.help_tree().find("Usage: archive files run") != std::string::npos,
        "styled/gray and recursive tree help");
  check(parser.complete({"files"}, "r") == std::vector<std::string>{"r", "run"}
        && parser.complete({"files", "r", "--format"}, "t") == std::vector<std::string>{"tar"}
        && parser.complete({"files", "r"}, "--format=z")
               == std::vector<std::string>{"--format=zip"}
        && parser.complete({"files", "r"}, "--no") == std::vector<std::string>{"--no-compress"}
        && parser.complete({"files", "r", "--"}, "--no").empty(),
        "completion follows hierarchy, aliases, operands, switches, and terminator");
}
} // namespace

int main()
{
  parsing();
  validation();
  help_and_completion();
}
