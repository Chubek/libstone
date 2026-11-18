#include "CLI.hpp"
#include "WIT-IDL.hpp"

int main(int argc, char** argv) {
    using namespace limestone::wasmutils;
    if (argc != 2) {
        std::cerr << "usage: wit2c INPUT\n";
        return 2;
    }
    try {
        detail::write_output(wit_to_c(detail::read_input(argv[1])));
    } catch (const WitError& error) {
        std::cerr << argv[1] << ':' << error.location.line << ':'
                  << error.location.column << ": error: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "wit2c: " << error.what() << '\n';
        return 1;
    }
}
