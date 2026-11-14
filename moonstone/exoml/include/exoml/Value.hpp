#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace moonstone::exoml {
// The first five alternatives preserve the original Value API.
using Value = std::variant<std::monostate, std::int64_t, bool, std::string,
                           std::vector<std::string>, std::uint64_t, double>;

std::string stringify(const Value &value);
std::string_view value_type(const Value &value) noexcept;
// Parses one complete literal. References are resolved by Interpreter.
Value parse_literal(std::string_view text);
} // namespace moonstone::exoml
