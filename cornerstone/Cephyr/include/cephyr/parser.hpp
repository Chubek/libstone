#pragma once

#include <memory>
#include <optional>

namespace cparser {
class ASTNode;
}

namespace cephyr {

class Diagnostics;
struct Preprocessed;

class ParsedUnit {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit ParsedUnit(std::unique_ptr<Impl>);
  friend std::optional<ParsedUnit> parse(const Preprocessed &, Diagnostics &);

public:
  ParsedUnit(ParsedUnit &&) noexcept;
  ParsedUnit &operator=(ParsedUnit &&) noexcept;
  ~ParsedUnit();

  ParsedUnit(const ParsedUnit &) = delete;
  ParsedUnit &operator=(const ParsedUnit &) = delete;

  // The tree and all its nodes are borrowed from this ParsedUnit.
  cparser::ASTNode *root() const;
};

std::optional<ParsedUnit> parse(const Preprocessed &, Diagnostics &);

} // namespace cephyr
