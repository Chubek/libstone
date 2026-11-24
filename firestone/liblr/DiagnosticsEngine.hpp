#pragma once

// Public entry point for rule-based lexer and parser diagnostics. Callers can
// dispatch directly on their own LRDiagState or use the bounded recovery drivers.
#include "DiagnosticsCore.hpp"
#include "DiagnosticsPatterns.hpp"
#include "DiagnosticsRecovery.hpp"
