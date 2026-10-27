#pragma once
#include "../libir/InterferenceGraph.hpp"
#include <cmath>
#include <functional>
#include <numeric>

namespace firestone::regalloc {
using ValueId = ir::ValueId;
using RegisterId = std::size_t;
using Position = std::size_t;
inline constexpr auto invalid_id = ir::invalid_id;

struct LiveRange {
    Position begin = 0, end = 0; // Half-open; uses occur before definitions at a boundary.
};
inline bool overlaps(LiveRange a, LiveRange b) { return a.begin < b.end && b.begin < a.end; }
inline bool overlaps(const std::vector<LiveRange>& a, const std::vector<LiveRange>& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (overlaps(a[i], b[j])) return true;
        if (a[i].end <= b[j].begin) ++i; else ++j;
    }
    return false;
}
struct PhysicalRegister {
    std::string name;
    std::set<std::string> classes{"general"};
    std::string bank = "general";
    // Allocation units model aliases, lanes and tuples. Empty means the register's ID.
    std::set<std::size_t> units;
    bool reserved = false;
    bool callee_saved = false;
    double cost = 0; // Per-value cost, not a once-per-function save cost.
};
struct Machine {
    std::vector<PhysicalRegister> registers;
    static Machine uniform(std::size_t count) {
        Machine m;
        for (std::size_t i = 0; i < count; ++i) {
            PhysicalRegister r; r.name = "r" + std::to_string(i); r.units.insert(i);
            m.registers.push_back(std::move(r));
        }
        return m;
    }
    bool aliases(RegisterId a, RegisterId b) const {
        const auto& x = registers.at(a).units; const auto& y = registers.at(b).units;
        if (x.empty()) return y.empty() ? a == b : y.count(a) != 0;
        if (y.empty()) return x.count(b) != 0;
        for (auto u : x) if (y.count(u)) return true;
        return false;
    }
};
struct Value {
    std::vector<LiveRange> ranges; // Sorted, nonempty segments; empty for graph-only inputs.
    std::vector<Position> uses; // Sorted; duplicates represent multiple operands.
    std::string reg_class = "general";
    std::optional<std::string> bank;
    std::optional<std::set<RegisterId>> allowed; // An empty set really means no registers.
    std::optional<RegisterId> fixed;
    std::vector<RegisterId> hints;
    double spill_cost = 1;
    double frequency = 1;
    std::size_t loop_depth = 0;
    bool spillable = true;
    std::optional<double> rematerialization_cost; // Caller guarantees recomputation is legal.
    std::size_t spill_size = 8, spill_alignment = 8;
};
struct Affinity { ValueId a = 0, b = 0; double weight = 1; };
class Problem {
public:
    ValueId add(Value value = {}) {
        validate_value(value);
        auto id = values_.size();
        values_.push_back(std::move(value)); edges_.emplace_back(); explicit_.emplace_back();
        for (ValueId v = 0; v < id; ++v) if (overlaps(values_[v].ranges, values_[id].ranges)) {
            edges_[v].insert(id); edges_[id].insert(v);
        }
        return id;
    }
    void interfere(ValueId a, ValueId b) {
        value(a); value(b);
        if (a != b) { edges_[a].insert(b); edges_[b].insert(a); explicit_[a].insert(b); explicit_[b].insert(a); }
    }
    void prefer(ValueId a, ValueId b, double weight = 1) {
        value(a); value(b);
        if (!std::isfinite(weight) || weight < 0) throw std::invalid_argument("invalid affinity weight");
        if (a != b) affinities_.push_back({a, b, weight});
    }
    const Value& value(ValueId v) const { return values_.at(v); }
    const std::set<ValueId>& neighbors(ValueId v) const { return edges_.at(v); }
    const std::set<ValueId>& explicit_neighbors(ValueId v) const { return explicit_.at(v); }
    bool interferes(ValueId a, ValueId b) const { return neighbors(a).count(b) != 0; }
    const std::vector<Affinity>& affinities() const { return affinities_; }
    std::size_t size() const { return values_.size(); }
private:
    static void validate_value(const Value& v) {
        if (!std::isfinite(v.spill_cost) || v.spill_cost < 0 || !std::isfinite(v.frequency) || v.frequency < 0 ||
            (v.rematerialization_cost && (!std::isfinite(*v.rematerialization_cost) || *v.rematerialization_cost < 0)) ||
            v.spill_size == 0 || v.spill_alignment == 0 || (v.spill_alignment & (v.spill_alignment - 1)))
            throw std::invalid_argument("invalid allocation value");
        Position end = 0;
        for (auto r : v.ranges) {
            if (r.begin >= r.end || r.begin < end) throw std::invalid_argument("invalid live ranges");
            end = r.end;
        }
        if (!std::is_sorted(v.uses.begin(), v.uses.end())) throw std::invalid_argument("unsorted uses");
        for (auto use : v.uses) {
            bool found = false;
            for (auto r : v.ranges) if (r.begin <= use && use < r.end) found = true;
            if (!found) throw std::invalid_argument("use outside live range");
        }
    }
    std::vector<Value> values_;
    std::vector<std::set<ValueId>> edges_, explicit_;
    std::vector<Affinity> affinities_;
};
enum class LocationKind { Unassigned, Register, Stack, Rematerialize };
struct Location {
    LocationKind kind = LocationKind::Unassigned;
    std::size_t index = invalid_id;
    static Location reg(RegisterId r) { return {LocationKind::Register, r}; }
    static Location stack(std::size_t s) { return {LocationKind::Stack, s}; }
    static Location rematerialize() { return {LocationKind::Rematerialize, invalid_id}; }
    bool operator==(const Location& other) const { return kind == other.kind && index == other.index; }
    bool operator!=(const Location& other) const { return !(*this == other); }
};
struct SpillSlot { std::size_t size = 0, alignment = 1; };
enum class Stop { Complete, NeedsRepair, Infeasible, SearchLimit };
struct Allocation {
    std::vector<Location> locations;
    std::vector<SpillSlot> slots;
    Stop stop = Stop::Complete;
    bool feasible = false;
    bool optimal = false;
    double cost = 0;
    std::size_t explored = 0;
};
inline bool legal(const Value& v, const Machine& m, RegisterId r) {
    if (r >= m.registers.size()) return false;
    const auto& p = m.registers[r];
    return !p.reserved && p.classes.count(v.reg_class) && (!v.bank || *v.bank == p.bank) &&
           (!v.allowed || v.allowed->count(r)) && (!v.fixed || *v.fixed == r);
}
inline std::vector<RegisterId> candidates(const Value& v, const Machine& m) {
    std::vector<RegisterId> out;
    for (RegisterId r = 0; r < m.registers.size(); ++r) if (legal(v, m, r)) out.push_back(r);
    return out;
}
inline void validate(const Problem& p, const Machine& m) {
    for (const auto& r : m.registers) if (!std::isfinite(r.cost) || r.cost < 0)
        throw std::invalid_argument("invalid physical register cost");
    for (ValueId v = 0; v < p.size(); ++v) {
        const auto& x = p.value(v);
        if (x.allowed) for (auto r : *x.allowed) if (r >= m.registers.size()) throw std::invalid_argument("unknown allowed register");
        for (auto r : x.hints) if (r >= m.registers.size()) throw std::invalid_argument("unknown register hint");
        if (x.fixed && !legal(x, m, *x.fixed)) throw std::invalid_argument("illegal precolor");
        if (x.fixed) for (auto n : p.neighbors(v)) if (p.value(n).fixed && m.aliases(*x.fixed, *p.value(n).fixed))
            throw std::invalid_argument("conflicting precolors");
    }
}
namespace detail {
inline double add_cost(double a, double b) {
    double c = a + b;
    if (!std::isfinite(c)) throw std::overflow_error("allocation cost overflow");
    return c;
}
inline double spill_cost(const Value& v) {
    return v.rematerialization_cost ? std::min(v.spill_cost, *v.rematerialization_cost) : v.spill_cost;
}
inline Location spill(const Value& v) {
    if (v.fixed || !v.spillable) return {};
    return v.rematerialization_cost && *v.rematerialization_cost <= v.spill_cost ? Location::rematerialize() : Location::stack(0);
}
inline bool fits(const Problem& p, const Machine& m, const std::vector<Location>& locations, ValueId v, RegisterId r) {
    if (!legal(p.value(v), m, r)) return false;
    for (auto n : p.neighbors(v)) if (locations[n].kind == LocationKind::Register && m.aliases(r, locations[n].index)) return false;
    return true;
}
inline std::vector<RegisterId> preferences(const Problem& p, const Machine& m, const std::vector<Location>& locations, ValueId v) {
    auto out = candidates(p.value(v), m);
    auto score = [&](RegisterId r) {
        double weight = 0;
        for (const auto& a : p.affinities()) {
            auto n = a.a == v ? a.b : a.b == v ? a.a : invalid_id;
            if (n != invalid_id && locations[n] == Location::reg(r)) weight = add_cost(weight, a.weight);
        }
        return weight;
    };
    std::stable_sort(out.begin(), out.end(), [&](auto a, auto b) {
        if (score(a) != score(b)) return score(a) > score(b);
        const auto& hints = p.value(v).hints;
        auto ha = std::find(hints.begin(), hints.end(), a), hb = std::find(hints.begin(), hints.end(), b);
        if (ha != hb) return ha < hb;
        return m.registers[a].cost < m.registers[b].cost;
    });
    return out;
}
inline Allocation initial(const Problem& p) {
    Allocation out; out.locations.resize(p.size());
    for (ValueId v = 0; v < p.size(); ++v) if (p.value(v).fixed) out.locations[v] = Location::reg(*p.value(v).fixed);
    return out;
}
inline double objective(const Problem& p, const Machine& m, const std::vector<Location>& locations) {
    double cost = 0;
    for (ValueId v = 0; v < p.size(); ++v) {
        auto l = locations[v];
        if (l.kind == LocationKind::Register) cost = add_cost(cost, m.registers[l.index].cost);
        if (l.kind == LocationKind::Stack) cost = add_cost(cost, p.value(v).spill_cost);
        if (l.kind == LocationKind::Rematerialize) cost = add_cost(cost, *p.value(v).rematerialization_cost);
    }
    for (const auto& a : p.affinities()) if (locations[a.a].kind != LocationKind::Register || locations[a.a] != locations[a.b])
        cost = add_cost(cost, a.weight);
    return cost;
}
inline void finish(const Problem& p, const Machine& m, Allocation& out) {
    out.slots.clear(); out.feasible = true;
    std::vector<std::vector<ValueId>> occupants;
    for (ValueId v = 0; v < p.size(); ++v) {
        auto& l = out.locations[v]; const auto& value = p.value(v);
        if (l.kind == LocationKind::Unassigned) out.feasible = false;
        if (l.kind != LocationKind::Stack) continue;
        std::size_t slot = 0;
        for (; slot < occupants.size(); ++slot) {
            bool conflict = false;
            for (auto n : occupants[slot]) if (p.interferes(v, n)) conflict = true;
            if (!conflict) break;
        }
        if (slot == occupants.size()) { occupants.emplace_back(); out.slots.push_back({}); }
        occupants[slot].push_back(v); l.index = slot;
        out.slots[slot].size = std::max(out.slots[slot].size, value.spill_size);
        out.slots[slot].alignment = std::max(out.slots[slot].alignment, value.spill_alignment);
    }
    out.cost = objective(p, m, out.locations);
    if (!out.feasible && out.stop == Stop::Complete) out.stop = Stop::NeedsRepair;
}
inline std::vector<ValueId> ids(const Problem& p) {
    std::vector<ValueId> out(p.size()); std::iota(out.begin(), out.end(), 0); return out;
}
} // namespace detail

// Independent check of the allocation, including stack-slot interference and objective.
inline std::vector<std::string> verify(const Problem& p, const Machine& m, const Allocation& a) {
    validate(p, m); std::vector<std::string> errors;
    if (a.locations.size() != p.size()) return {"allocation size mismatch"};
    bool valid = true;
    for (ValueId v = 0; v < p.size(); ++v) {
        auto l = a.locations[v]; const auto& x = p.value(v);
        bool ok = false;
        switch (l.kind) {
        case LocationKind::Register: ok = legal(x, m, l.index); break;
        case LocationKind::Stack:
            ok = x.spillable && !x.fixed && l.index < a.slots.size();
            if (ok) ok = a.slots[l.index].size >= x.spill_size && a.slots[l.index].alignment >= x.spill_alignment &&
                         a.slots[l.index].alignment % x.spill_alignment == 0;
            break;
        case LocationKind::Rematerialize: ok = x.spillable && !x.fixed && x.rematerialization_cost.has_value(); break;
        case LocationKind::Unassigned: break;
        }
        if (!ok) { errors.push_back("illegal or missing location for value " + std::to_string(v)); valid = false; }
    }
    if (valid) {
        for (ValueId v = 0; v < p.size(); ++v) for (auto n : p.neighbors(v)) if (n > v) {
            auto x = a.locations[v], y = a.locations[n];
            if ((x.kind == LocationKind::Register && y.kind == LocationKind::Register && m.aliases(x.index, y.index)) ||
                (x.kind == LocationKind::Stack && y.kind == LocationKind::Stack && x.index == y.index)) errors.push_back("interfering locations");
        }
        auto cost = detail::objective(p, m, a.locations);
        if (!std::isfinite(a.cost) || std::abs(cost - a.cost) > 1e-9 * std::max(1.0, cost)) errors.push_back("incorrect allocation cost");
    }
    if (!a.feasible) errors.push_back("allocation has no feasible result");
    if (a.stop == Stop::Infeasible || a.stop == Stop::NeedsRepair) errors.push_back("allocation did not complete");
    if (a.optimal && a.stop != Stop::Complete) errors.push_back("unproved optimality");
    return errors;
}
} // namespace firestone::regalloc
