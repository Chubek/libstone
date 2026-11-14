#pragma once

// Nerotypical — constraint-based type analysis for compilers and
// interpreters, powered by the vendored Satie solver.
//
//   #include <nerotypical/Nerotypical.hpp>
//
// pulls in everything: the shared analysis vocabulary, both solving modes
// and the demo-language checkers.  See nerotypical/README.md for the
// embedding contract and the honest limitations list.

#include "Analysis.hpp"
#include "Language.hpp"
#include "SetTyping.hpp"
#include "StructuralTyping.hpp"
