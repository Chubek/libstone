#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace novostone::action {
enum class Target { C, Python, Rust, Pascal, Latex };
enum class Dialect { One, Two };
struct ParseError : std::invalid_argument {
  std::string source;
  std::size_t offset, line, column;
  ParseError(std::string source, std::string_view text, std::size_t offset, const std::string& message);
};
namespace detail { struct Spec; }
// The two-argument generate() selects the specification's required dialect.
// Explicit options select a capability/accounting mode over the same runtime;
// dialect Two may also execute legacy specifications; dialect One rejects
// extended ones. Budget overrides (zero keeps the specification default)
// apply to dialect 2 only. Invalid or oversized budgets throw invalid_argument.
struct Options {
  Dialect dialect = Dialect::One;
  std::int64_t steps = 0;
  int nodes = 0, symbols = 0, refs = 0, depth = 0, heap = 0;
};
// Immutable, validated specification. Generated implementation details stay private.
class Language {
public:
  const std::string& name() const;
  // The dialect a specification requires: Two once it uses dialect-2-only
  // constructs (closures, continuations, generators, ...), One otherwise.
  Dialect default_dialect() const;
private:
  explicit Language(std::shared_ptr<const detail::Spec> spec);
  std::shared_ptr<const detail::Spec> spec_;
  friend Language parse(std::string_view, std::string);
  friend std::string generate(const Language&, Target);
  friend std::string generate(const Language&, Target, const Options&);
};
// Source is limited to 64 KiB, 256 constructors, 16 fields per constructor,
// and 4096 lowered dialect-1 instructions (8192 in dialect 2). Errors carry
// one-based byte locations.
Language parse(std::string_view text, std::string source = "<acsem>");
Target target(std::string_view name);
// Emits a standalone stdin-to-stdout interpreter, or a complete LaTeX document.
std::string generate(const Language& language, Target target);
std::string generate(const Language& language, Target target, const Options& options);
} // namespace novostone::action
