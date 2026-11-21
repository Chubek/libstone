#include "novostone/ActionSemantics/ActionSemantics.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int parse_number(const std::string& text) {
  std::size_t at = 0;
  long long value = 0;
  try { value = std::stoll(text, &at); }
  catch (const std::exception&) { throw std::invalid_argument("invalid budget value: " + text); }
  if (at != text.size() || value <= 0 || value > std::numeric_limits<int>::max())
    throw std::invalid_argument("invalid budget value: " + text);
  return static_cast<int>(value);
}
}

int main(int argc, char** argv) {
  using namespace novostone::action;
  try {
    Options options;
    bool dialect_forced = false, check = false, emit = false, help = false;
    bool output_selected = false, positional_only = false;
    std::string selected_name, output_path;
    std::vector<std::string> paths;
    for (int at = 1; at < argc; ++at) {
      std::string argument = argv[at];
      if (positional_only) { paths.push_back(argument); continue; }
      if (argument == "--") { positional_only = true; continue; }
      auto equals = argument.find('=');
      auto key = argument.substr(0, equals);
      auto value = [&]() -> std::string {
        if (equals != std::string::npos) return argument.substr(equals + 1);
        if (++at >= argc) throw std::invalid_argument("missing value for " + key);
        return argv[at];
      };
      if (argument == "--help" || argument == "-h") {
        help = true;
      } else if (argument == "--check") {
        if (check || emit) throw std::invalid_argument("choose one of --check and --target/--emit");
        check = true;
      } else if (key == "--target" || key == "--emit") {
        if (check || emit) throw std::invalid_argument("choose one of --check and --target/--emit");
        selected_name = value(); emit = true;
      } else if (key == "-o" || key == "--output") {
        if (output_selected) throw std::invalid_argument("output specified more than once");
        output_path = value(); output_selected = true;
      } else if (key == "--dialect") {
        auto d = value();
        if (d == "1") options.dialect = Dialect::One;
        else if (d == "2") options.dialect = Dialect::Two;
        else throw std::invalid_argument("unknown dialect (expected 1 or 2)");
        dialect_forced = true;
      } else if (key == "--define") {
        auto def = value();
        auto separator = def.find('=');
        if (separator == std::string::npos) throw std::invalid_argument("invalid definition: " + def);
        auto budget = def.substr(0, separator);
        auto n = parse_number(def.substr(separator + 1));
        if (budget == "steps") options.steps = n;
        else if (budget == "nodes") options.nodes = n;
        else if (budget == "symbols") options.symbols = n;
        else if (budget == "refs") options.refs = n;
        else if (budget == "depth") options.depth = n;
        else if (budget == "heap") options.heap = n;
        else throw std::invalid_argument("unknown budget key: " + budget);
      } else if (!argument.empty() && argument.front() == '-') {
        throw std::invalid_argument("unknown option: " + argument);
      } else {
        paths.push_back(argument);
      }
    }
    if (help) {
      std::cout << "Usage: acsem --check FILE.acsem\n"
                   "       acsem [--dialect 1|2] [--define KEY=VALUE]... --target c|python|rust|pascal|latex FILE.acsem [-o FILE]\n"
                   "--emit aliases --target; --output aliases -o. Options may precede or follow FILE.\n"
                   "Long value options also accept --name=value. Use -- before a filename beginning with '-'.\n"
                   "Dialect 2 is selected automatically for specifications that need it.\n"
                   "Budget keys (dialect 2 only): steps, nodes, symbols, refs, depth, heap.\n";
      return 0;
    }
    if ((!check && !emit) || paths.size() != 1)
      throw std::invalid_argument("usage: acsem --check FILE | --target/--emit TARGET FILE [-o/--output FILE]");
    if (check && output_selected) throw std::invalid_argument("--output is not supported with --check");
    auto selected = check ? Target::Latex : target(selected_name);
    const auto& path = paths.front();
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    std::string text(65537, '\0'); file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.bad()) throw std::runtime_error("cannot read " + path);
    text.resize(static_cast<std::size_t>(file.gcount()));
    auto language = parse(text, path);
    if (check) { std::cout << language.name() << ": valid\n"; return 0; }
    if (!dialect_forced) options.dialect = language.default_dialect();
    if (options.dialect == Dialect::One && (options.steps || options.nodes || options.symbols || options.refs || options.depth || options.heap))
      throw std::invalid_argument("budget overrides require --dialect 2");
    auto output = generate(language, selected, options);
    if (output_selected) {
      std::ofstream destination(output_path, std::ios::binary);
      if (!destination || !(destination << output)) throw std::runtime_error("cannot write output");
      destination.close();
      if (!destination) throw std::runtime_error("cannot close output");
    } else if (!(std::cout << output)) throw std::runtime_error("cannot write stdout");
    return 0;
  } catch (const std::exception& e) { std::cerr << "acsem: " << e.what() << '\n'; return 2; }
}
