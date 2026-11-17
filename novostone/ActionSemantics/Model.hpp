#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace novostone::action::detail {
// Stable internal instruction order shared by the four bundled runtimes.
enum ExprOp { Integer, Field, Token, Given, Bound, Contents, Add, Sub, Mul, Div, Equal, Less };
enum ActionOp { Complete, Fail, Give, Check, Escape, Bind, Store, Allocate, Evaluate,
                Then, AndThen, Hence, Otherwise, Trapping, Furthermore };
struct Instruction { int op = 0, a = 0, b = 0; std::int64_t value = 0; };
struct FieldSpec { std::string name; int type; }; // 0 integer, 1 identifier, 2+ sort
struct Constructor { std::string name; int sort, rule = -1; std::vector<FieldSpec> fields; };
struct Model {
  std::string name, source;
  int start = 0;
  std::vector<std::string> sorts, tokens;
  std::vector<Constructor> constructors;
  std::vector<Instruction> expressions, actions;
};
} // namespace novostone::action::detail
