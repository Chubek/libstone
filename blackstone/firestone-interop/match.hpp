#pragma once
#include <librewrite/Term.hpp>

namespace blackstone {
using Term = firestone::rewrite::Term;
using Pattern = firestone::rewrite::Pattern;
using Substitution = firestone::rewrite::Substitution;
Pattern parse_pattern(const std::string& source);
std::optional<Substitution> match(const Pattern& pattern, const Term& term);
} // namespace blackstone
