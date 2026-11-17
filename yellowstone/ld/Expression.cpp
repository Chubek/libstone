#include "Expression.h"
#include "parser.hpp"
#include <mutex>
#include <stdexcept>

namespace qld::script {
namespace {
ExprPtr translate(const void *node, unsigned depth = 0) {
  if (!node || depth > 256) throw std::runtime_error("expression nesting limit exceeded");
  const auto v = qld_expression_inspect(node);
  const std::string text = v.text ? std::string(v.text, v.length) : "";
  switch (v.kind) {
    case QLD_NUMBER: {
      uint64_t value;
      if (!parseScriptNumber(text, value)) throw std::runtime_error("invalid script number: " + text);
      return std::make_unique<NumberExpr>(value);
    }
    case QLD_SYMBOL: return std::make_unique<SymbolExpr>(text);
    case QLD_DOT: return std::make_unique<DotExpr>();
    case QLD_UNARY: return std::make_unique<UnaryExpr>(text, translate(v.first, depth + 1));
    case QLD_BINARY: return std::make_unique<BinaryExpr>(text, translate(v.first, depth + 1), translate(v.second, depth + 1));
    case QLD_CONDITIONAL:
      return std::make_unique<TernaryExpr>(translate(v.first, depth + 1), translate(v.second, depth + 1), translate(v.third, depth + 1));
    case QLD_CALL: {
      std::vector<ExprPtr> args;
      for (size_t i = 0; i < v.count; ++i) args.push_back(translate(qld_expression_argument(node, i), depth + 1));
      return std::make_unique<CallExpr>(text, std::move(args));
    }
  }
  throw std::runtime_error("invalid generated expression");
}
}
ExprPtr parseExpression(const char *input, size_t& consumed) {
  static std::mutex parserMutex;
  std::unique_ptr<qld_expression, decltype(&qld_expression_close)> tree(nullptr, qld_expression_close);
  {
    // Aurocks has process-local error/depth state; never race two invocations.
    std::lock_guard lock(parserMutex);
    tree.reset(qld_expression_open(input, &consumed));
  }
  if (!tree) throw std::runtime_error("invalid or excessively complex linker expression");
  return translate(qld_expression_root(tree.get()));
}
}
