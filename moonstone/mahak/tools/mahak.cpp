// Entry point for the Mahak command-line tool. The behavior lives in
// src/Cli.cpp so tests can exercise the CLI without starting a new process.
#include "mahak/Cli.hpp"

int main(int argc, char **argv) {
    return moonstone::mahak::cli_main(argc, argv);
}
