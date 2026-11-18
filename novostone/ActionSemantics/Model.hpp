#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace novostone::action::detail {
// Stable internal instruction order shared by the four bundled runtimes.
enum ExprOp { Integer, Field, Token, Given, Bound, Contents, Add, Sub, Mul, Div, Equal, Less,
              Arity, Component, StructuralEqual, Identical };
enum ActionOp { Complete, Fail, Give, Check, Escape, Bind, Store, Allocate, Evaluate,
                Then, AndThen, Hence, Otherwise, Trapping, Furthermore,
                Closure = 16, Apply = 17, Return = 18, TailCall = 19,
                Enact = 20, LetRec = 21, CallCC = 23, ResumeCont = 24,
                Unfolding = 100, Unfold = 101, Put = 102,
                If = 103, While = 104, Break = 105, Continue = 106, TailUnfold = 107,
                CreateGenerator = 110, YieldValue = 111, Next = 112, Close = 113,
                Construct = 114, Match = 115, Deconstruct = 116 };
enum PatternOp { AnyPattern, BindPattern, IntPattern, TokenPattern, ConstructorPattern,
                 AsPattern, OrPattern };
// Match operands: subject expression (a), optional data sort (b, -1 absent),
// and pool triples [pattern index, guard expression or -1, body action].
// ConstructorPattern stores its constructor (a) and a pool slice of child IDs.
// A data field type is integer (0), identifier (1), datum (2), or data sort (3+).
enum DataType { DataInteger, DataToken, DataAny, FirstDataSort };
struct VariantSpec {
  std::string name;
  int sort, fields, arity; // field type indices are in Model::pool
};
// Numeric opcodes are shared by all runtime templates; gaps are reserved.
struct Instruction {
  int op = -1, a = -1, b = -1;
  std::int64_t value = 0;
  int pool = -1, count = 0; // variadic operand slice in Model::pool
};
struct FieldSpec { std::string name; int type; }; // 0 integer, 1 identifier, 2+ sort
struct Constructor { std::string name; int sort, rule = -1; std::vector<FieldSpec> fields; };
struct Budgets {
  std::int64_t steps = 100000;
  int nodes = 2048, symbols = 256, refs = 1024;
  int data_items = 64, bindings = 64, depth = 128, heap = 4096, parse_depth = 128;
};
struct Model {
  std::string name, source;
  int start = 0;
  std::vector<std::string> sorts, tokens;
  std::vector<Constructor> constructors;
  std::vector<Instruction> expressions, actions;
  std::vector<std::string> data_sorts;
  std::vector<VariantSpec> variants;
  std::vector<Instruction> patterns;
  std::vector<int> pool;
  Budgets budgets;
  int int_width = 32;
  bool requires_extended = false, tagged_cells = false;
};
// Dialects are compatibility/capability modes over this one canonical model.
struct Spec {
  std::shared_ptr<const Model> model;
};
} // namespace novostone::action::detail
