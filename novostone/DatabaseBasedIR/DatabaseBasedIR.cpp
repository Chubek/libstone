#include "novostone/DatabaseBasedIR/DatabaseBasedIR.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <sstream>
#include <tuple>
#include <charconv>
#include <locale>

namespace novostone::ir {
namespace {
const std::vector<std::string> names{"param", "const", "copy", "add", "sub", "mul", "eq", "ult",
                                      "select", "load", "store", "jump", "branch", "return", "phi"};
std::int64_t integer(const datalog::Value& value) { return std::get<std::int64_t>(value); }
std::uint32_t word(const datalog::Value& value) {
  auto v = integer(value);
  if (v < 0 || v > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("i32 immediate out of range");
  return static_cast<std::uint32_t>(v);
}
bool terminator(Op op) { return op == Op::Jump || op == Op::Branch || op == Op::Return; }
std::size_t operand_count(Op op) {
  switch (op) {
    case Op::Copy: case Op::Load: case Op::Branch: case Op::Return: return 1;
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Eq: case Op::Ult: case Op::Store: return 2;
    case Op::Select: return 3;
    default: return 0;
  }
}
const std::vector<std::string> base_relations{"Signature", "Block", "Entry", "Instr", "Operand", "Target"};
bool unsigned_integer(std::istream& in, std::uint64_t& value) {
  std::string token;
  if (!(in >> token)) return false;
  auto result = std::from_chars(token.data(), token.data() + token.size(), value);
  return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}
}
std::string name(Op op) { return names.at(static_cast<std::size_t>(op)); }
Op opcode(const std::string& text) {
  auto it = std::find(names.begin(), names.end(), text);
  if (it == names.end()) throw std::invalid_argument("unknown opcode: " + text);
  return static_cast<Op>(it - names.begin());
}
bool pure(Op op) { return op <= Op::Select || op == Op::Phi; }
bool produces_value(Op op) { return op <= Op::Load || op == Op::Phi; }
Effects effects(Op op) { return {op == Op::Load, op == Op::Store, op == Op::Load || op == Op::Store}; }
datalog::Program program() {
  using namespace datalog;
  auto I = Type::Integer; auto S = Type::Symbol;
  Program p;
  p.declare("Signature", {I}); p.declare("Block", {I}); p.declare("Entry", {I});
  p.declare("Instr", {I, I, I, S, I});
  p.declare("Operand", {I, I, I}); p.declare("Target", {I, I, I});
  p.declare("Guard", {I, I, I, I, I, I});
  p.declare("Assumption", {I, I, S});
  p.declare("Frame", {I, I, I}); p.declare("Binding", {I, I});
  p.declare("Uses", {I, I}, true); p.declare("Used", {I}, true);
  p.declare("Pure", {I}, true); p.declare("Dead", {I}, true);
  p.declare("Edge", {I, I}, true); p.declare("Reach", {I, I}, true);
  p.declare("Constant", {I, I}, true); p.declare("Binary", {I, S, I, I}, true);
  p.declare("Fold", {I, S, I, I}, true);
  p.declare("CopyCandidate", {I, S, I}, true); p.declare("ConstCandidate", {I, S, I}, true);
  p.declare("SelectValue", {I, I, I, I}, true); p.declare("CopyValue", {I, I}, true);
  p.declare("BranchValue", {I, I, I, I}, true);
  p.declare("Effect", {I, S}, true);
  p.declare("Needed", {I}, true); p.declare("DeadGroup", {I}, true);
  p.declare("Phi", {I}, true); p.declare("BlockDef", {I, I}, true);
  p.declare("UseAt", {I, I}, true); p.declare("LocalUse", {I, I}, true);
  p.declare("PhiUse", {I, I, I}, true);
  p.declare("LiveIn", {I, I}, true); p.declare("LiveOut", {I, I}, true);
  p.declare("LiveEdge", {I, I, I}, true);
  auto v = Term::var; auto c = Term::constant; auto a = Term::any;
  p.add({{"Uses", {v("u"), v("v")}}, {{"Operand", {v("u"), a(), v("v")}}}});
  p.add({{"Used", {v("v")}}, {{"Uses", {a(), v("v")}}}});
  p.add({{"Used", {v("v")}}, {{"Binding", {a(), v("v")}}}});
  for (int op = 0; op <= static_cast<int>(Op::Select); ++op)
    p.add({{"Pure", {v("i")}}, {{"Instr", {v("i"), a(), a(), c(name(static_cast<Op>(op))), a()}}}});
  p.add({{"Pure", {v("i")}}, {{"Instr", {v("i"), a(), a(), c(std::string("phi")), a()}}}});
  p.add({{"Dead", {v("i")}}, {{"Pure", {v("i")}}, {"Used", {v("i")}, true}}});
  p.add({{"Needed", {v("v")}}, {{"Binding", {a(), v("v")}}}});
  p.add({{"Needed", {v("v")}}, {{"Instr", {v("i"), a(), a(), a(), a()}}, {"Pure", {v("i")}, true}, {"Uses", {v("i"), v("v")}}}});
  p.add({{"Needed", {v("v")}}, {{"Needed", {v("i")}}, {"Uses", {v("i"), v("v")}}}});
  p.add({{"DeadGroup", {v("i")}}, {{"Pure", {v("i")}}, {"Needed", {v("i")}, true}}});
  p.add({{"Phi", {v("i")}}, {{"Instr", {v("i"), a(), a(), c(std::string("phi")), a()}}}});
  for (int op = 0; op <= static_cast<int>(Op::Phi); ++op) if (produces_value(static_cast<Op>(op)))
    p.add({{"BlockDef", {v("b"), v("i")}}, {{"Instr", {v("i"), v("b"), a(), c(name(static_cast<Op>(op))), a()}}}});
  p.add({{"UseAt", {v("i"), v("v")}}, {{"Operand", {v("i"), a(), v("v")}}, {"Phi", {v("i")}, true}}});
  p.add({{"UseAt", {v("i"), v("v")}}, {{"Guard", {a(), v("i"), a(), a(), a(), v("f")}}, {"Binding", {v("f"), v("v")}}}});
  p.add({{"LocalUse", {v("b"), v("v")}}, {{"Instr", {v("i"), v("b"), a(), a(), a()}}, {"UseAt", {v("i"), v("v")}},
    {"BlockDef", {v("b"), v("v")}, true}}});
  p.add({{"PhiUse", {v("pred"), v("b"), v("v")}},
    {{"Instr", {v("i"), v("b"), a(), c(std::string("phi")), a()}}, {"Operand", {v("i"), v("slot"), v("v")}},
     {"Target", {v("i"), v("slot"), v("pred")}}}});
  p.add({{"LiveIn", {v("b"), v("v")}}, {{"LocalUse", {v("b"), v("v")}}}});
  p.add({{"LiveIn", {v("b"), v("v")}}, {{"LiveOut", {v("b"), v("v")}}, {"BlockDef", {v("b"), v("v")}, true}}});
  p.add({{"LiveEdge", {v("b"), v("s"), v("v")}}, {{"Edge", {v("b"), v("s")}}, {"LiveIn", {v("s"), v("v")}}}});
  p.add({{"LiveEdge", {v("b"), v("s"), v("v")}}, {{"Edge", {v("b"), v("s")}}, {"PhiUse", {v("b"), v("s"), v("v")}}}});
  p.add({{"LiveOut", {v("b"), v("v")}}, {{"LiveEdge", {v("b"), a(), v("v")}}}});
  for (auto op : {Op::Jump, Op::Branch})
    p.add({{"Edge", {v("b"), v("t")}}, {{"Instr", {v("i"), v("b"), a(), c(name(op)), a()}}, {"Target", {v("i"), a(), v("t")}}}});
  for (auto op : {Op::Load, Op::Store}) {
    p.add({{"Effect", {v("i"), c(std::string("trap"))}}, {{"Instr", {v("i"), a(), a(), c(name(op)), a()}}}});
    p.add({{"Effect", {v("i"), c(std::string(op == Op::Load ? "read" : "write"))}},
      {{"Instr", {v("i"), a(), a(), c(name(op)), a()}}}});
  }
  p.add({{"Reach", {v("a"), v("b")}}, {{"Edge", {v("a"), v("b")}}}});
  p.add({{"Reach", {v("a"), v("c")}}, {{"Reach", {v("a"), v("b")}}, {"Edge", {v("b"), v("c")}}}});
  p.add({{"Constant", {v("i"), v("c")}}, {{"Instr", {v("i"), a(), a(), c(std::string("const")), v("c")}}}});
  for (auto op : {Op::Add, Op::Sub, Op::Mul, Op::Eq, Op::Ult})
    p.add({{"Binary", {v("i"), c(name(op)), v("l"), v("r")}},
      {{"Instr", {v("i"), a(), a(), c(name(op)), a()}},
       {"Operand", {v("i"), c(std::int64_t{0}), v("l")}}, {"Operand", {v("i"), c(std::int64_t{1}), v("r")}}}});
  p.add({{"Fold", {v("i"), v("op"), v("a"), v("b")}},
    {{"Binary", {v("i"), v("op"), v("l"), v("r")}},
     {"Constant", {v("l"), v("a")}}, {"Constant", {v("r"), v("b")}}}});
  // Pattern, precondition and replacement relations keep policy in the query
  // layer. The rewrite executor only materializes these typed opportunities.
  for (const auto& [op, rule, identity] : std::vector<std::tuple<Op, std::string, std::int64_t>>{
         {Op::Add, "add_zero", 0}, {Op::Sub, "sub_zero", 0}, {Op::Mul, "mul_one", 1}}) {
    p.add({{"CopyCandidate", {v("i"), c(rule), v("l")}},
      {{"Binary", {v("i"), c(name(op)), v("l"), v("r")}}, {"Constant", {v("r"), c(identity)}}}});
    if (op != Op::Sub) p.add({{"CopyCandidate", {v("i"), c(rule), v("r")}},
      {{"Binary", {v("i"), c(name(op)), v("l"), v("r")}}, {"Constant", {v("l"), c(identity)}}}});
  }
  for (const auto& [op, rule, result] : std::vector<std::tuple<Op, std::string, std::int64_t>>{
         {Op::Sub, "sub_self", 0}, {Op::Eq, "eq_self", 1}, {Op::Ult, "ult_self", 0}})
    p.add({{"ConstCandidate", {v("i"), c(rule), c(result)}}, {{"Binary", {v("i"), c(name(op)), v("x"), v("x")}}}});
  for (const auto& side : {std::string("l"), std::string("r")})
    p.add({{"ConstCandidate", {v("i"), c(std::string("mul_zero")), c(std::int64_t{0})}},
      {{"Binary", {v("i"), c(std::string("mul")), v("l"), v("r")}}, {"Constant", {v(side), c(std::int64_t{0})}}}});
  p.add({{"SelectValue", {v("i"), v("condition"), v("yes"), v("no")}},
    {{"Instr", {v("i"), a(), a(), c(std::string("select")), a()}},
     {"Operand", {v("i"), c(std::int64_t{0}), v("condition")}},
     {"Operand", {v("i"), c(std::int64_t{1}), v("yes")}},
     {"Operand", {v("i"), c(std::int64_t{2}), v("no")}}}});
  p.add({{"CopyCandidate", {v("i"), c(std::string("select_value")), v("value")}},
    {{"SelectValue", {v("i"), a(), v("value"), v("value")}}}});
  p.add({{"CopyCandidate", {v("i"), c(std::string("select_value")), v("no")}},
    {{"SelectValue", {v("i"), v("condition"), a(), v("no")}}, {"Constant", {v("condition"), c(std::int64_t{0})}}}});
  p.add({{"CopyCandidate", {v("i"), c(std::string("select_value")), v("yes")}},
    {{"SelectValue", {v("i"), v("condition"), v("yes"), a()}}, {"Constant", {v("condition"), a()}},
     {"Constant", {v("condition"), c(std::int64_t{0})}, true}}});
  p.add({{"CopyValue", {v("i"), v("value")}},
    {{"Instr", {v("i"), a(), a(), c(std::string("copy")), a()}}, {"Operand", {v("i"), c(std::int64_t{0}), v("value")}}}});
  p.add({{"BranchValue", {v("i"), v("condition"), v("yes"), v("no")}},
    {{"Instr", {v("i"), a(), a(), c(std::string("branch")), a()}},
     {"Operand", {v("i"), c(std::int64_t{0}), v("condition")}},
     {"Target", {v("i"), c(std::int64_t{0}), v("yes")}},
     {"Target", {v("i"), c(std::int64_t{1}), v("no")}}}});
  return p;
}
Function load(const ccdb::Snapshot& snapshot) {
  Function f;
  if (snapshot.relation("Signature").size() != 1 || snapshot.relation("Entry").size() != 1)
    throw std::invalid_argument("function requires one signature and entry");
  f.arity = word(snapshot.relation("Signature").begin()->at(0));
  f.entry = integer(snapshot.relation("Entry").begin()->at(0));
  for (const auto& row : snapshot.relation("Block")) {
    auto id = integer(row[0]); f.blocks.emplace(id, Block{id, {}});
  }
  std::map<Id, std::map<std::int64_t, Instruction>> ordered;
  std::map<Id, Instruction*> instructions;
  for (const auto& row : snapshot.relation("Instr")) {
    auto id = integer(row[0]), block = integer(row[1]), position = integer(row[2]);
    if (!f.blocks.count(block) || position < 0) throw std::invalid_argument("invalid instruction block or position");
    auto [it, inserted] = ordered[block].emplace(position, Instruction{id, opcode(std::get<std::string>(row[3])), word(row[4]), {}, {}});
    if (!inserted || !instructions.emplace(id, &it->second).second) throw std::invalid_argument("duplicate instruction or position");
  }
  for (const auto& relation : {std::string("Operand"), std::string("Target")}) {
    std::map<Id, std::map<std::int64_t, Id>> slots;
    for (const auto& row : snapshot.relation(relation)) {
      auto id = integer(row[0]), index = integer(row[1]);
      if (!instructions.count(id) || index < 0 || !slots[id].emplace(index, integer(row[2])).second)
        throw std::invalid_argument("invalid or duplicate operand/target slot");
    }
    for (const auto& [id, entries] : slots) {
      auto& output = relation == "Operand" ? instructions.at(id)->operands : instructions.at(id)->targets;
      for (const auto& [index, value] : entries) {
        if (index != static_cast<std::int64_t>(output.size())) throw std::invalid_argument("non-contiguous operand/target slots");
        output.push_back(value);
      }
    }
  }
  for (auto& [id, entries] : ordered) for (auto& [position, instruction] : entries) {
    if (position != static_cast<std::int64_t>(f.blocks.at(id).instructions.size())) throw std::invalid_argument("non-contiguous instructions");
    f.blocks.at(id).instructions.push_back(std::move(instruction));
  }
  return f;
}
std::map<Id, std::set<Id>> dominators(const Function& f) {
  std::set<Id> all;
  std::map<Id, std::set<Id>> predecessors, dom;
  for (const auto& [id, block] : f.blocks) {
    all.insert(id);
    if (!block.instructions.empty()) for (auto target : block.instructions.back().targets) predecessors[target].insert(id);
  }
  for (auto id : all) dom[id] = id == f.entry ? std::set<Id>{id} : all;
  bool changed;
  do {
    changed = false;
    for (auto id : all) if (id != f.entry) {
      auto next = all;
      for (auto pred : predecessors[id]) {
        std::set<Id> intersection;
        std::set_intersection(next.begin(), next.end(), dom.at(pred).begin(), dom.at(pred).end(),
                              std::inserter(intersection, intersection.end()));
        next = std::move(intersection);
      }
      next.insert(id);
      if (next != dom[id]) { dom[id] = std::move(next); changed = true; }
    }
  } while (changed);
  return dom;
}
void verify(const Function& f) {
  if (!f.blocks.count(f.entry)) throw std::invalid_argument("missing entry block");
  std::set<Id> ids;
  std::map<Id, std::pair<Id, std::size_t>> defs;
  std::map<Id, std::set<Id>> predecessors;
  for (const auto& [id, block] : f.blocks) {
    if (id <= 0 || id != block.id || !ids.insert(id).second) throw std::invalid_argument("invalid block ID");
  }
  for (const auto& [id, block] : f.blocks) {
    if (block.instructions.empty() || !terminator(block.instructions.back().op)) throw std::invalid_argument("missing terminator");
    bool past_phis = false;
    for (std::size_t pos = 0; pos < block.instructions.size(); ++pos) {
      const auto& i = block.instructions[pos];
      (void)name(i.op);
      if (i.id <= 0 || !ids.insert(i.id).second) throw std::invalid_argument("duplicate or invalid entity ID");
      if (terminator(i.op) && pos + 1 != block.instructions.size()) throw std::invalid_argument("early terminator");
      if (i.op == Op::Phi) {
        if (past_phis || id == f.entry || i.operands.empty() || i.operands.size() != i.targets.size())
          throw std::invalid_argument("phi requires an incoming edge list at a non-entry block start");
      } else {
        past_phis = true;
        if (i.operands.size() != operand_count(i.op)) throw std::invalid_argument("incorrect operand count");
      }
      auto targets = i.op == Op::Jump ? 1u : i.op == Op::Branch ? 2u : 0u;
      if (i.op != Op::Phi && i.targets.size() != targets) throw std::invalid_argument("incorrect target count");
      for (auto target : i.targets) if (!f.blocks.count(target)) throw std::invalid_argument("missing branch target");
      if (terminator(i.op)) for (auto target : i.targets) predecessors[target].insert(id);
      if (i.op == Op::Param && (id != f.entry || i.immediate >= f.arity)) throw std::invalid_argument("invalid parameter");
      if (i.op != Op::Const && i.op != Op::Param && i.immediate != 0) throw std::invalid_argument("unexpected immediate");
      if (produces_value(i.op)) defs.emplace(i.id, std::make_pair(id, pos));
    }
  }
  std::set<Id> reachable{f.entry}; std::vector<Id> work{f.entry};
  while (!work.empty()) {
    auto id = work.back(); work.pop_back();
    for (auto target : f.blocks.at(id).instructions.back().targets) if (reachable.insert(target).second) work.push_back(target);
  }
  if (reachable.size() != f.blocks.size()) throw std::invalid_argument("unreachable block");
  auto dom = dominators(f);
  for (const auto& [id, block] : f.blocks) for (std::size_t pos = 0; pos < block.instructions.size(); ++pos) {
    const auto& i = block.instructions[pos];
    if (i.op == Op::Phi && (std::set<Id>(i.targets.begin(), i.targets.end()) != predecessors[id] ||
                           i.targets.size() != predecessors[id].size()))
      throw std::invalid_argument("phi must name every predecessor exactly once");
    for (std::size_t slot = 0; slot < i.operands.size(); ++slot) {
      auto value = i.operands[slot];
      auto it = defs.find(value);
      if (it == defs.end()) throw std::invalid_argument("undefined SSA value");
      auto [def_block, def_pos] = it->second;
      if (i.op == Op::Phi) {
        auto pred = i.targets[slot];
        if (!dom.at(pred).count(def_block)) throw std::invalid_argument("phi value does not dominate incoming edge");
      } else if (!dom.at(id).count(def_block) || (def_block == id && def_pos >= pos))
        throw std::invalid_argument("SSA value does not dominate use");
    }
  }
}
void verify(const ccdb::Snapshot& snapshot) { verify(load(snapshot)); }
std::vector<MemoryDependency> memory_dependencies(const Function& f) {
  verify(f);
  std::vector<MemoryDependency> result;
  for (const auto& [id, block] : f.blocks) {
    Id previous = 0;
    for (const auto& i : block.instructions) {
      auto effect = effects(i.op);
      if (!effect.reads && !effect.writes) continue;
      result.push_back({id, i.id, previous, effect}); previous = i.id;
    }
  }
  return result;
}
void store(ccdb::Transaction& transaction, const Function& f) {
  verify(f);
  auto before = transaction.preview();
  for (const auto& relation : base_relations) for (const auto& row : before.relation(relation)) transaction.erase(relation, row);
  transaction.insert("Signature", {static_cast<std::int64_t>(f.arity)});
  transaction.insert("Entry", {f.entry});
  for (const auto& [id, block] : f.blocks) {
    transaction.reserve(id);
    transaction.insert("Block", {id});
    for (std::size_t pos = 0; pos < block.instructions.size(); ++pos) {
      const auto& i = block.instructions[pos];
      transaction.reserve(i.id);
      transaction.insert("Instr", {i.id, id, static_cast<std::int64_t>(pos), name(i.op), static_cast<std::int64_t>(i.immediate)});
      for (std::size_t slot = 0; slot < i.operands.size(); ++slot)
        transaction.insert("Operand", {i.id, static_cast<std::int64_t>(slot), i.operands[slot]});
      for (std::size_t slot = 0; slot < i.targets.size(); ++slot)
        transaction.insert("Target", {i.id, static_cast<std::int64_t>(slot), i.targets[slot]});
    }
  }
}
ccdb::Snapshot publish(ccdb::Database& database, const ccdb::Snapshot& base, const Function& function, const std::string& reason) {
  auto transaction = database.begin(base);
  store(transaction, function);
  return transaction.commit(reason, [](const auto& s) { verify(s); });
}
Builder::Builder(ccdb::Database& database, std::uint32_t arity) : database_(database), base_(database.snapshot()) { function_.arity = arity; }
Id Builder::block() {
  auto id = database_.allocate(); function_.blocks.emplace(id, Block{id, {}});
  if (!function_.entry) function_.entry = id;
  return id;
}
Id Builder::emit(Id block, Op op, std::vector<Id> operands, std::uint32_t immediate, std::vector<Id> targets) {
  auto id = database_.allocate();
  function_.blocks.at(block).instructions.push_back({id, op, immediate, std::move(operands), std::move(targets)});
  return id;
}
void Builder::incoming(Id phi, Id predecessor, Id value) {
  for (auto& [id, block] : function_.blocks) {
    (void)id;
    for (auto& i : block.instructions) if (i.id == phi) {
      if (i.op != Op::Phi) throw std::invalid_argument("incoming requires a phi instruction");
      i.targets.push_back(predecessor); i.operands.push_back(value); return;
    }
  }
  throw std::invalid_argument("unknown phi instruction");
}
ccdb::Snapshot Builder::finish() { return publish(database_, base_, function_, "build function"); }
std::string dump(const Function& f) {
  verify(f);
  std::ostringstream out;
  out.imbue(std::locale::classic());
  bool phis = false;
  for (const auto& [id, block] : f.blocks) { (void)id; for (const auto& i : block.instructions) phis |= i.op == Op::Phi; }
  out << "novostone " << (phis ? 2 : 1) << ' ' << f.arity << ' ' << f.entry << '\n';
  for (const auto& [id, block] : f.blocks) {
    out << "block " << id << '\n';
    for (const auto& i : block.instructions) {
      out << "instr " << i.id << ' ' << name(i.op) << ' ' << i.immediate << ' ' << i.operands.size();
      for (auto value : i.operands) out << ' ' << value;
      out << ' ' << i.targets.size();
      for (auto target : i.targets) out << ' ' << target;
      out << '\n';
    }
  }
  return out.str();
}
Function parse(const std::string& text) {
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  std::string token; std::uint64_t version, arity;
  Function f;
  if (!(in >> token) || !unsigned_integer(in, version) || !unsigned_integer(in, arity) || !(in >> f.entry) ||
      token != "novostone" || (version != 1 && version != 2) || arity > UINT32_MAX)
    throw std::invalid_argument("invalid IR header");
  f.arity = static_cast<std::uint32_t>(arity);
  Block* block = nullptr;
  while (in >> token) {
    if (token == "block") {
      Id id;
      if (!(in >> id)) throw std::invalid_argument("invalid block");
      auto [it, inserted] = f.blocks.emplace(id, Block{id, {}});
      if (!inserted) throw std::invalid_argument("duplicate block");
      block = &it->second;
    } else if (token == "instr" && block) {
      Instruction i; std::string op; std::uint64_t immediate, count;
      if (!(in >> i.id >> op) || !unsigned_integer(in, immediate) || !unsigned_integer(in, count) ||
          immediate > UINT32_MAX || count > text.size()) throw std::invalid_argument("invalid instruction");
      i.op = opcode(op); i.immediate = static_cast<std::uint32_t>(immediate);
      if ((i.op == Op::Phi && version < 2) || (i.op != Op::Phi && count > 3)) throw std::invalid_argument("invalid operand count or dialect version");
      for (std::uint64_t n = 0; n < count; ++n) { Id value; if (!(in >> value)) throw std::invalid_argument("invalid operand"); i.operands.push_back(value); }
      if (!unsigned_integer(in, count) || count > text.size() || (i.op != Op::Phi && count > 2)) throw std::invalid_argument("invalid targets");
      for (std::uint64_t n = 0; n < count; ++n) { Id target; if (!(in >> target)) throw std::invalid_argument("invalid target"); i.targets.push_back(target); }
      block->instructions.push_back(std::move(i));
    } else throw std::invalid_argument("unexpected IR token: " + token);
  }
  verify(f);
  return f;
}
Outcome resume(const Function& f, const std::vector<std::uint32_t>& args, State state, std::size_t budget, Hook hook) {
  verify(f);
  Outcome out{std::nullopt, {}, std::move(state), false};
  if (args.size() != f.arity) { out.trap = "argument count"; return out; }
  auto& s = out.state;
  for (;;) {
    if (s.steps >= budget) { out.trap = "step budget"; return out; }
    const auto& block = f.blocks.at(s.block);
    const auto& i = block.instructions.at(s.offset);
    if (s.offset == 0 && i.op == Op::Phi && s.phi_values.empty()) {
      for (const auto& phi : block.instructions) {
        if (phi.op != Op::Phi) break;
        auto edge = std::find(phi.targets.begin(), phi.targets.end(), s.predecessor);
        if (edge == phi.targets.end()) throw std::invalid_argument("resume missing phi predecessor");
        s.phi_values.emplace(phi.id, s.values.at(phi.operands.at(static_cast<std::size_t>(edge - phi.targets.begin()))));
      }
    }
    if (hook && !hook(i, s)) { out.suspended = true; return out; }
    ++s.steps;
    auto operand = [&](std::size_t index) { return s.values.at(i.operands.at(index)); };
    std::uint32_t value = 0;
    switch (i.op) {
      case Op::Param: value = args.at(i.immediate); break;
      case Op::Phi: value = s.phi_values.at(i.id); s.phi_values.erase(i.id); break;
      case Op::Const: value = i.immediate; break;
      case Op::Copy: value = operand(0); break;
      case Op::Add: value = static_cast<std::uint32_t>(std::uint64_t{operand(0)} + operand(1)); break;
      case Op::Sub: value = static_cast<std::uint32_t>(std::uint64_t{operand(0)} - operand(1)); break;
      case Op::Mul: value = static_cast<std::uint32_t>(std::uint64_t{operand(0)} * operand(1)); break;
      case Op::Eq: value = operand(0) == operand(1); break;
      case Op::Ult: value = operand(0) < operand(1); break;
      case Op::Select: value = operand(operand(0) ? 1 : 2); break;
      case Op::Load: case Op::Store: {
        auto address = operand(0);
        if (address >= s.memory.size()) { out.trap = "memory bounds"; return out; }
        if (i.op == Op::Load) value = s.memory[address];
        else { s.memory[address] = operand(1); s.writes.push_back({address, operand(1)}); }
        break;
      }
      case Op::Jump: case Op::Branch: {
        auto target = i.targets[i.op == Op::Jump || operand(0) ? 0 : 1];
        s.predecessor = s.block; s.block = target; s.offset = 0; s.phi_values.clear(); continue;
      }
      case Op::Return: out.value = operand(0); return out;
    }
    if (produces_value(i.op)) s.values[i.id] = value;
    ++s.offset;
  }
}
Outcome evaluate(const Function& f, const std::vector<std::uint32_t>& args, std::vector<std::uint32_t> memory, std::size_t budget) {
  State state; state.block = f.entry; state.memory = std::move(memory);
  return resume(f, args, std::move(state), budget);
}
bool equivalent(const Outcome& a, const Outcome& b) {
  return a.value == b.value && a.trap == b.trap && a.suspended == b.suspended &&
    a.state.memory == b.state.memory && a.state.writes == b.state.writes;
}
} // namespace novostone::ir
