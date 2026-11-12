#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <variant>
#include <vector>

namespace novostone::datalog {
using Value = std::variant<std::int64_t, std::string>;
using Tuple = std::vector<Value>;
using Relation = std::set<Tuple>;
using Facts = std::map<std::string, Relation>;
enum class Type { Integer, Symbol };
struct Term {
  enum class Kind { Variable, Constant, Wildcard };
  Kind kind;
  Value value;
  static Term var(std::string name);
  static Term constant(Value value);
  static Term any();
};
struct Atom {
  std::string relation;
  std::vector<Term> terms;
  bool negated = false;
};
struct Rule { Atom head; std::vector<Atom> body; };
struct Schema { std::vector<Type> columns; bool derived = false; };
struct Limits { std::size_t facts = 1000000; std::size_t work = 10000000; };
class BudgetExceeded : public std::runtime_error { public: using std::runtime_error::runtime_error; };
enum class Evaluation { SemiNaive, Naive };
struct Statistics {
  std::size_t rounds = 0;
  std::size_t rule_evaluations = 0;
  std::size_t tuple_visits = 0;
  std::size_t index_builds = 0;
};
struct Result {
  Relation rows;
  std::set<std::string> dependencies;
  std::size_t work = 0;
  Statistics statistics;
};

// Finite, function-free, typed Datalog with safe stratified negation.
class Program {
public:
  void declare(std::string name, std::vector<Type> columns, bool derived = false);
  void add(Rule rule);
  void validate() const;
  void validate_fact(const std::string& relation, const Tuple& tuple) const;
  const std::map<std::string, Schema>& schemas() const { return schemas_; }
  // Canonical complete schema/rule identity, not a hash or an executable parser.
  std::string signature() const;
  std::set<std::string> dependencies(const std::string& goal) const;
  Result evaluate(const Facts& facts, const std::string& goal, Limits limits = {},
                  Evaluation evaluation = Evaluation::SemiNaive) const;
private:
  std::map<std::string, std::size_t> strata() const;
  std::map<std::string, Schema> schemas_;
  std::vector<Rule> rules_;
};
std::string dump(const Relation& relation);
} // namespace novostone::datalog
