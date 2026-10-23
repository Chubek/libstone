#include "ArchBFDApp.hpp"
#include <iostream>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " OBJECT-OR-ARCHIVE...\n";
    return 2;
  }
  int result = 0;
  for (int i = 1; i < argc; ++i) {
    try {
      archbfd::visitObjects(argv[i], [](std::string_view name, const archbfd::Binary& file) {
        auto info = file.info();
        std::cout << name << ": " << archbfd_format_name(info.format) << ' '
                  << archbfd_arch_name(info.arch) << '\n';
        for (const auto& section : file.sections())
          std::cout << "  " << section.index << ' ' << archbfd::text(section.name)
                    << " size=" << section.size << " file=" << section.file_size << '\n';
        for (const auto& symbol : file.symbols())
          if (!(symbol.flags & ARCHBFD_SYM_UNDEFINED))
            std::cout << "  symbol " << archbfd::text(symbol.name) << '\n';
      });
    } catch (const std::exception& e) {
      std::cerr << argv[i] << ": " << e.what() << '\n';
      result = 1;
    }
  }
  return result;
}
