#pragma once

// Public entry point for rule-based lexer and parser diagnostics, including
// compiled byte PEGs. Callers can dispatch directly on their own LRDiagState
// or use the bounded recovery drivers. Parser internals stay with the caller.
#include "DiagnosticsCore.hpp"
#include "DiagnosticsPatterns.hpp"
#include "DiagnosticsRecovery.hpp"
