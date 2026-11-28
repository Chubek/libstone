#include "mahak/coordination/Blotter.hpp"

#include <iostream>
#include <string>

using namespace moonstone::mahak::coordination;

int main(int argc, char **argv) {
    std::string root;
    bool snapshot = false;
    std::string destination;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--root" && index + 1 < argc)
            root = argv[++index];
        else if (argument == "--snapshot" && index + 1 < argc) {
            snapshot = true;
            destination = argv[++index];
        } else {
            std::cerr << "usage: mahak-coord-status_main --root DIR [--snapshot DIR]\n";
            return 2;
        }
    }
    if (root.empty()) {
        std::cerr << "--root is required\n";
        return 2;
    }
    if (snapshot) {
        auto attached = Blotter::attach(root);
        if (!attached) {
            std::cerr << attached.error().message << "\n";
            return 1;
        }
        auto result = attached.value().snapshot(destination);
        if (!result) {
            std::cerr << result.error().message << "\n";
            return 1;
        }
        std::cout << "snapshot written to " << destination << "\n";
        return 0;
    }
    auto inspection = Blotter::inspect(root);
    if (!inspection) {
        std::cerr << inspection.error().message << "\n";
        return 1;
    }
    std::cout << inspection.value().dump(true) << "\n";
    return 0;
}
