#include <fstream>
#include <iostream>
#include <iterator>
#include <supersonic/Supersonic.hpp>
#include <syren/Syren.hpp>
int main(int argc, char **argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "diagnose") {
            std::ifstream file(argv[2], std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot open rule file");
            std::string text((std::istreambuf_iterator<char>(file)), {});
            moonstone::syren::Catalog c;
            c.add(moonstone::syren::parse_rule(text));
            auto ds = c.diagnose(moonstone::sexpr::Parser(argv[3]).one());
            for (auto &d : ds)
                std::cout << moonstone::format(d) << '\n';
            return ds.empty() ? 0 : 2;
        }
        if (argc == 2 && std::string(argv[1]) == "synthesize-demo") {
            using namespace moonstone::supersonic;
            Problem p;
            p.grammar.variables = {"x", "y"};
            p.grammar.constants = {0, 1};
            p.examples = {{{{"x", 2}, {"y", 3}}, 5}, {{{"x", 9}, {"y", 4}}, 13}};
            auto s = solve(p);
            if (s.program) {
                std::cout << s.program->print() << '\n';
                return 0;
            }
            std::cerr << "no solution within bounds\n";
            return 1;
        }
        std::cerr << "usage: moonstone-cli diagnose RULE.sexp '(tree ...)' | synthesize-demo\n";
        return 64;
    } catch (const std::exception &e) {
        std::cerr << "moonstone: " << e.what() << '\n';
        return 1;
    }
}
