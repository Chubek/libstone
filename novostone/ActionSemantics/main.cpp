#include "novostone/ActionSemantics/ActionSemantics.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

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
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << "Usage: acsem --check FILE.acsem\n"
                   "       acsem [--dialect 1|2] [--define KEY=VALUE]... --target c|python|rust|pascal|latex FILE.acsem [-o FILE]\n"
                   "Dialect 2 is selected automatically for specifications that need it.\n"
                   "Budget keys (dialect 2 only): steps, nodes, symbols, refs, depth, heap.\n";
      return 0;
    }
    int at = 1;
    Options options;
    bool dialect_forced = false;
    while (at < argc) {
      if (std::string_view(argv[at]) == "--dialect") {
        if (at + 1 >= argc) throw std::invalid_argument("usage: --dialect 1|2");
        std::string_view d = argv[at + 1];
        if (d == "1") options.dialect = Dialect::One;
        else if (d == "2") options.dialect = Dialect::Two;
        else throw std::invalid_argument("unknown dialect (expected 1 or 2)");
        dialect_forced = true; at += 2;
      } else if (std::string_view(argv[at]) == "--define") {
        if (at + 1 >= argc) throw std::invalid_argument("usage: --define KEY=VALUE");
        std::string def = argv[at + 1];
        auto equals = def.find('=');
        if (equals == std::string::npos) throw std::invalid_argument("invalid definition: " + def);
        auto key = def.substr(0, equals), value = def.substr(equals + 1);
        auto n = parse_number(value);
        if (key == "steps") options.steps = n;
        else if (key == "nodes") options.nodes = n;
        else if (key == "symbols") options.symbols = n;
        else if (key == "refs") options.refs = n;
        else if (key == "depth") options.depth = n;
        else if (key == "heap") options.heap = n;
        else throw std::invalid_argument("unknown budget key: " + key);
        at += 2;
      } else break;
    }
    bool check = at + 1 < argc && std::string_view(argv[at]) == "--check" && at + 2 == argc;
    bool emit = at + 2 < argc && std::string_view(argv[at]) == "--target";
    if (!check && !emit)
      throw std::invalid_argument(
        "usage: acsem --check FILE | [--dialect 1|2] [--define KEY=VALUE]... --target TARGET FILE [-o FILE]");
    if (emit) {
      // --target TARGET FILE [-o FILE]: remaining args after --target
      if (!(argc - at == 3 || (argc - at == 5 && std::string_view(argv[at + 3]) == "-o")))
        throw std::invalid_argument(
          "usage: acsem --check FILE | [--dialect 1|2] [--define KEY=VALUE]... --target TARGET FILE [-o FILE]");
    }
    auto selected = check ? Target::Latex : target(argv[at + 1]);
    std::string path = argv[check ? at + 1 : at + 2];
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
    if (argc - at == 5) {
      std::ofstream destination(argv[at + 4], std::ios::binary);
      if (!destination || !(destination << output)) throw std::runtime_error("cannot write output");
      destination.close();
      if (!destination) throw std::runtime_error("cannot close output");
    } else if (!(std::cout << output)) throw std::runtime_error("cannot write stdout");
    return 0;
  } catch (const std::exception& e) { std::cerr << "acsem: " << e.what() << '\n'; return 2; }
}
