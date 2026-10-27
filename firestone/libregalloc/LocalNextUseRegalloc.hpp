#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
struct LocalAccess {
    std::vector<ValueId> uses, definitions;
};
enum class LocalActionKind { Load, Store, Rematerialize };
struct LocalAction {
    LocalActionKind kind;
    std::size_t instruction;
    ValueId value;
    RegisterId reg;
};
struct LocalAllocation {
    // Unique spill homes are value IDs. Initial live-ins reside in their homes.
    std::vector<std::map<ValueId, RegisterId>> registers;
    std::vector<LocalAction> actions; // Loads before the instruction; final stores at instructions.size().
    bool feasible = true;
};
struct LocalNextUseRegalloc {
    // Straight-line, single-definition block. All operands and results must coexist
    // (conservative for two-address targets). Belady's furthest-next-use eviction.
    LocalAllocation allocate(const std::vector<LocalAccess>& instructions, std::size_t value_count,
                             std::size_t register_count, const std::set<ValueId>& live_out = {}) const {
        std::set<ValueId> defined;
        for (auto v : live_out) if (v >= value_count) throw std::invalid_argument("unknown live-out value");
        std::vector<std::vector<std::size_t>> uses(value_count);
        std::vector<std::size_t> definition(value_count, invalid_id);
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            for (auto v : instructions[i].uses) {
                if (v >= value_count) throw std::invalid_argument("unknown local use");
                uses[v].push_back(i);
            }
            for (auto v : instructions[i].definitions) {
                if (v >= value_count || !defined.insert(v).second) throw std::invalid_argument("invalid local definition");
                definition[v] = i;
            }
        }
        for (ValueId v = 0; v < value_count; ++v) if (definition[v] != invalid_id)
            for (auto i : uses[v]) if (i <= definition[v]) throw std::invalid_argument("local use before definition");
        for (auto v : live_out) uses[v].push_back(instructions.size());
        LocalAllocation out; out.registers.resize(instructions.size());
        std::map<ValueId, RegisterId> resident; std::set<ValueId> dirty;
        auto next_use = [&](ValueId v, std::size_t i) {
            auto next = std::upper_bound(uses[v].begin(), uses[v].end(), i);
            return next == uses[v].end() ? invalid_id : *next;
        };
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            std::set<ValueId> required(instructions[i].uses.begin(), instructions[i].uses.end());
            required.insert(instructions[i].definitions.begin(), instructions[i].definitions.end());
            if (required.size() > register_count) { out.feasible = false; return out; }
            for (auto v : required) if (!resident.count(v)) {
                std::set<RegisterId> occupied;
                for (auto item : resident) occupied.insert(item.second);
                RegisterId r = 0; while (r < register_count && occupied.count(r)) ++r;
                if (r == register_count) {
                    ValueId victim = invalid_id; std::size_t furthest = 0;
                    for (auto item : resident) if (!required.count(item.first)) {
                        auto next = next_use(item.first, i);
                        if (victim == invalid_id || next > furthest) { victim = item.first; furthest = next; }
                    }
                    if (victim == invalid_id) { out.feasible = false; return out; }
                    r = resident.at(victim);
                    if (dirty.count(victim) && furthest != invalid_id) out.actions.push_back({LocalActionKind::Store, i, victim, r});
                    resident.erase(victim); dirty.erase(victim);
                }
                resident[v] = r;
                if (definition[v] != i) out.actions.push_back({LocalActionKind::Load, i, v, r});
            }
            for (auto v : instructions[i].definitions) dirty.insert(v);
            for (auto v : required) out.registers[i][v] = resident.at(v);
            for (auto it = resident.begin(); it != resident.end();) {
                if (next_use(it->first, i) == invalid_id) { dirty.erase(it->first); it = resident.erase(it); } else ++it;
            }
        }
        for (auto v : live_out) if (dirty.count(v)) out.actions.push_back({LocalActionKind::Store, instructions.size(), v, resident.at(v)});
        return out;
    }
};
} // namespace firestone::regalloc
