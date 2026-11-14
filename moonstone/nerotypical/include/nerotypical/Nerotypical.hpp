#pragma once
// Nerotypical — original Satie-backed type analysis plus Sequent IR.
// These includes are source-compatible with the upstream 2026-11-14 API.
#include "Analysis.hpp"
#include "Language.hpp"
#include "SetTyping.hpp"
#include "StructuralTyping.hpp"
#include "sequent/SequentIR.hpp"
// Opt-in integration headers (require the upstream language/Satie types):
//   #include <nerotypical/sequent/Bridge.hpp>
//   #include <nerotypical/sequent/TypeAdapters.hpp>
