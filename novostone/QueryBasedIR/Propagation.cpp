#include "novostone/QueryBasedIR/QueryBasedIR.hpp"

#include <deque>
#include <sstream>
#include <locale>

namespace novostone::query {
std::string Propagation::explain() const {
  std::ostringstream out; out.imbue(std::locale::classic());
  out << "conditional propagation work " << work << "\nexecutable blocks";
  for (auto block : blocks) out << ' ' << block;
  out << '\n';
  for (const auto& [from, to] : edges) out << "executable edge " << from << " -> " << to << '\n';
  for (const auto& [id, value] : values) {
    out << '%' << id << ' ';
    if (value.kind == ValueKind::Constant) out << "constant " << value.value;
    else out << (value.kind == ValueKind::Unknown ? "unknown" : "overdefined");
    out << '\n';
  }
  return out.str();
}
namespace {
AbstractValue known(std::uint32_t value) { return {ValueKind::Constant, value}; }
AbstractValue top() { return {ValueKind::Overdefined, 0}; }
AbstractValue meet(AbstractValue a, AbstractValue b) {
  if (a.kind == ValueKind::Unknown) return b;
  if (b.kind == ValueKind::Unknown || a == b) return a;
  return top();
}
AbstractValue binary(ir::Op op, ir::Id left, ir::Id right, AbstractValue a, AbstractValue b) {
  using ir::Op;
  if (left == right) {
    if (op == Op::Sub || op == Op::Ult) return known(0);
    if (op == Op::Eq) return known(1);
  }
  if (op == Op::Mul && ((a.kind == ValueKind::Constant && a.value == 0) ||
                        (b.kind == ValueKind::Constant && b.value == 0))) return known(0);
  if (a.kind == ValueKind::Unknown || b.kind == ValueKind::Unknown) return {};
  if (a.kind == ValueKind::Overdefined || b.kind == ValueKind::Overdefined) return top();
  switch (op) {
    case Op::Add: return known(static_cast<std::uint32_t>(std::uint64_t{a.value} + b.value));
    case Op::Sub: return known(static_cast<std::uint32_t>(std::uint64_t{a.value} - b.value));
    case Op::Mul: return known(static_cast<std::uint32_t>(std::uint64_t{a.value} * b.value));
    case Op::Eq: return known(a.value == b.value);
    case Op::Ult: return known(a.value < b.value);
    default: throw std::logic_error("unsupported SCCP binary opcode");
  }
}
}
Propagation propagate(const ir::Function& function, std::size_t budget) {
  ir::verify(function);
  Propagation result;
  auto tick = [&] {
    if (result.work >= budget) throw datalog::BudgetExceeded("conditional propagation work budget exceeded");
    ++result.work;
  };
  std::map<ir::Id, const ir::Instruction*> instructions;
  std::map<ir::Id, ir::Id> owner;
  std::map<ir::Id, std::set<ir::Id>> users;
  for (const auto& [block, body] : function.blocks) for (const auto& i : body.instructions) {
    tick(); instructions[i.id] = &i; owner[i.id] = block;
    if (ir::produces_value(i.op)) result.values[i.id];
    for (auto operand : i.operands) { tick(); users[operand].insert(i.id); }
  }
  std::deque<ir::Id> work;
  std::set<ir::Id> queued;
  auto enqueue = [&](ir::Id id) { tick(); if (queued.insert(id).second) work.push_back(id); };
  auto activate = [&](ir::Id from, ir::Id to) {
    tick();
    if (!result.edges.insert({from, to}).second) return;
    bool fresh = result.blocks.insert(to).second;
    for (const auto& i : function.blocks.at(to).instructions) {
      if (!fresh && i.op != ir::Op::Phi) break;
      enqueue(i.id);
    }
  };
  result.blocks.insert(function.entry);
  for (const auto& i : function.blocks.at(function.entry).instructions) enqueue(i.id);
  while (!work.empty()) {
    tick(); auto id = work.front(); work.pop_front(); queued.erase(id);
    auto block = owner.at(id);
    if (!result.blocks.count(block)) continue;
    const auto& i = *instructions.at(id);
    auto operand = [&](std::size_t slot) { return result.values.at(i.operands.at(slot)); };
    if (i.op == ir::Op::Jump) { activate(block, i.targets[0]); continue; }
    if (i.op == ir::Op::Branch) {
      auto condition = operand(0);
      if (i.targets[0] == i.targets[1]) activate(block, i.targets[0]);
      else if (condition.kind == ValueKind::Constant) activate(block, i.targets[condition.value ? 0 : 1]);
      else if (condition.kind == ValueKind::Overdefined) { activate(block, i.targets[0]); activate(block, i.targets[1]); }
      continue;
    }
    if (!ir::produces_value(i.op)) continue;
    AbstractValue value;
    switch (i.op) {
      case ir::Op::Param: case ir::Op::Load: value = top(); break;
      case ir::Op::Const: value = known(i.immediate); break;
      case ir::Op::Copy: value = operand(0); break;
      case ir::Op::Phi:
        for (std::size_t slot = 0; slot < i.operands.size(); ++slot) {
          tick(); if (result.edges.count({i.targets[slot], block})) value = meet(value, operand(slot));
        }
        break;
      case ir::Op::Select: {
        auto condition = operand(0), yes = operand(1), no = operand(2);
        if (condition.kind == ValueKind::Constant) value = condition.value ? yes : no;
        else if (yes == no) value = yes;
        else if (condition.kind == ValueKind::Overdefined) value = meet(yes, no);
        break;
      }
      default: value = binary(i.op, i.operands[0], i.operands[1], operand(0), operand(1)); break;
    }
    auto next = meet(result.values.at(id), value);
    if (next != result.values.at(id)) {
      result.values.at(id) = next;
      for (auto user : users[id]) enqueue(user);
    }
  }
  return result;
}
} // namespace novostone::query
