#include "blackstone.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: blackstone-kernels DIRECTORY [--ir IR | --phase PHASE]\n";
        return 2;
    }
    try {
        if (argc != 2 && (argc != 4 || (std::string(argv[2]) != "--ir" && std::string(argv[2]) != "--phase")))
            throw std::invalid_argument("expected --ir IR or --phase PHASE");
        blackstone::Runtime runtime; runtime.load_directory(argv[1]);
        for (const auto& kernel : runtime.kernels()) {
            if (argc == 4 && ((std::string(argv[2]) == "--ir" ? kernel.ir : kernel.phase) != argv[3])) continue;
            std::cout << kernel.name << "\t" << kernel.ir << "\t" << kernel.phase << "\n";
            std::cout << "  requires:";
            for (const auto& analysis : kernel.requirements) std::cout << " " << analysis;
            std::cout << "\n  preserves:";
            for (const auto& analysis : kernel.preserves) std::cout << " " << analysis;
            std::cout << "\n  " << kernel.source << "\n";
            if (!kernel.documentation.empty()) std::cout << "  " << kernel.documentation << "\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "blackstone: " << e.what() << "\n"; return 1; }
}
