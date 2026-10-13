#include "match.hpp"
namespace blackstone {
Pattern parse_pattern(const std::string& source) {
    auto pattern = firestone::rewrite::parse(source);
    firestone::rewrite::validate_term(pattern, true);
    return pattern;
}
}
