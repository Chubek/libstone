#pragma once
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace novostone::action {
enum class Target { C, Python, Rust, Pascal, Latex };
struct ParseError : std::invalid_argument {
  std::string source;
  std::size_t offset, line, column;
  ParseError(std::string source, std::string_view text, std::size_t offset, const std::string& message);
};
namespace detail { struct Model; }
// Immutable, validated specification. Generated implementation details stay private.
class Language {
public:
  const std::string& name() const;
private:
  explicit Language(std::shared_ptr<const detail::Model> model);
  std::shared_ptr<const detail::Model> model_;
  friend Language parse(std::string_view, std::string);
  friend std::string generate(const Language&, Target);
};
// Source is limited to 64 KiB, 256 constructors, 16 fields per constructor,
// and 4096 lowered instructions. Errors carry one-based byte locations.
Language parse(std::string_view text, std::string source = "<acsem>");
Target target(std::string_view name);
// Emits a standalone stdin-to-stdout interpreter, or a complete LaTeX document.
std::string generate(const Language& language, Target target);
} // namespace novostone::action
