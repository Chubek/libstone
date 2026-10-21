#pragma once

// TRS terms have the same syntax and matching semantics as Firestone's
// existing first-order rewriting terms. Keep the two APIs interoperable.
#include "../librewrite/Term.hpp"

namespace firestone::trs {
using rewrite::Term;
using rewrite::Pattern;
using rewrite::Substitution;
using rewrite::TermHash;
using rewrite::is_variable;
using rewrite::validate_term;
using rewrite::variables;
using rewrite::match;
using rewrite::match_into;
using rewrite::substitute;
using rewrite::parse;
using rewrite::to_string;
} // namespace firestone::trs
