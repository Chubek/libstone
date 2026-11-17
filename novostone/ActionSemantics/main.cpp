#include "novostone/ActionSemantics/ActionSemantics.hpp"
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  using namespace novostone::action;
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout << "Usage: acsem --check FILE.acsem\n"
                   "       acsem --target c|python|rust|pascal|latex FILE.acsem [-o FILE]\n";
      return 0;
    }
    bool check = argc == 3 && std::string_view(argv[1]) == "--check";
    if (!check && !((argc == 4 || argc == 6) && std::string_view(argv[1]) == "--target" &&
        (argc != 6 || std::string_view(argv[4]) == "-o")))
      throw std::invalid_argument("usage: acsem --check FILE | --target TARGET FILE [-o FILE]");
    auto selected = check ? Target::Latex : target(argv[2]);
    std::string path = argv[check ? 2 : 3];
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path);
    std::string text(65537, '\0'); file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.bad()) throw std::runtime_error("cannot read " + path);
    text.resize(static_cast<std::size_t>(file.gcount()));
    auto language = parse(text, path);
    if (check) { std::cout << language.name() << ": valid\n"; return 0; }
    auto output = generate(language, selected);
    if (argc == 6) {
      std::ofstream destination(argv[5], std::ios::binary);
      if (!destination || !(destination << output)) throw std::runtime_error("cannot write output");
      destination.close();
      if (!destination) throw std::runtime_error("cannot close output");
    } else if (!(std::cout << output)) throw std::runtime_error("cannot write stdout");
    return 0;
  } catch (const std::exception& e) { std::cerr << "acsem: " << e.what() << '\n'; return 2; }
}
