#ifndef HEADSTONE_SMTLIB2_UMBRELLA_HPP
#define HEADSTONE_SMTLIB2_UMBRELLA_HPP
// Public umbrella: no link library and no repository includes are required.
#include "diagnostic.hpp"
#include "lexer.hpp"
#include "sexpr.hpp"
#include "solver.hpp"
#include "incremental_sat.hpp"
#include "session.hpp"
#include "ast.hpp"
#include "theory.hpp"
#include "bitvector.hpp"
#include "builder.hpp"
#include "protocol.hpp"

#if defined(SMTLIB2_CLI) && defined(SMTLIB2_REPL)
#error "Select only one of SMTLIB2_CLI and SMTLIB2_REPL per executable"
#elif defined(SMTLIB2_CLI) || defined(SMTLIB2_REPL)
#include "frontend.hpp"
int main(int argc, char** argv) {
#ifdef SMTLIB2_REPL
    constexpr bool interactive = true;
#else
    constexpr bool interactive = false;
#endif
    return smtlib2::detail::frontend_main(argc, argv, interactive);
}
#endif
#endif
