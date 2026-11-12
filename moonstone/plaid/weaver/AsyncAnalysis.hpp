#pragma once
#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace moonstone::plaid::weaver {
using BlockId = unsigned;
struct Operation {
    std::vector<std::string> uses;
    std::vector<std::string> defines;
    bool suspension_point = false;
};
struct BasicBlock {
    BlockId id = 0;
    std::vector<Operation> operations;
    std::vector<BlockId> successors;
};
struct AsyncFunction {
    BlockId entry = 0;
    std::vector<BasicBlock> blocks;
};
struct Suspension {
    BlockId block = 0;
    std::size_t operation = 0;
    std::set<std::string> saved_values;
};
struct FrameLayout {
    std::vector<std::string> slots;
    std::vector<Suspension> suspensions;
};

// Backward may-liveness, with fixed-point propagation over CFG. Defined values
// used only before suspension are excluded from the spill layout.
inline FrameLayout analyze_frame(const AsyncFunction& function) {
    if (function.blocks.empty()) throw std::invalid_argument("async function has no blocks");
    std::map<BlockId, const BasicBlock*> blocks;
    for (const auto& b : function.blocks) {
        if (!blocks.emplace(b.id, &b).second) throw std::invalid_argument("duplicate async block");
    }
    if (!blocks.contains(function.entry)) throw std::invalid_argument("invalid async entry");
    for (const auto& b : function.blocks)
        for (auto next : b.successors)
            if (!blocks.contains(next)) throw std::invalid_argument("invalid async successor");
    std::map<BlockId, std::set<std::string>> in, out;
    bool changed = true;
    std::size_t rounds = 0;
    while (changed) {
        changed = false;
        if (++rounds > 100000) throw std::runtime_error("async liveness budget exceeded");
        for (auto it = function.blocks.rbegin(); it != function.blocks.rend(); ++it) {
            std::set<std::string> next;
            for (auto s : it->successors)
                next.insert(in[s].begin(), in[s].end());
            auto live = next;
            for (auto op = it->operations.rbegin(); op != it->operations.rend(); ++op) {
                for (const auto& d : op->defines) live.erase(d);
                live.insert(op->uses.begin(), op->uses.end());
            }
            if (in[it->id] != live || out[it->id] != next) {
                in[it->id] = std::move(live);
                out[it->id] = std::move(next);
                changed = true;
            }
        }
    }
    FrameLayout layout;
    std::set<std::string> all;
    for (const auto& b : function.blocks) {
        auto live = out[b.id];
        for (std::size_t j = b.operations.size(); j-- > 0;) {
            const auto& op = b.operations[j];
            if (op.suspension_point) {
                layout.suspensions.push_back({b.id, j, live});
                all.insert(live.begin(), live.end());
            }
            for (const auto& d : op.defines) live.erase(d);
            live.insert(op.uses.begin(), op.uses.end());
        }
    }
    layout.slots.assign(all.begin(), all.end());
    std::sort(layout.suspensions.begin(), layout.suspensions.end(), [](const auto& a, const auto& b) {
        return std::pair{a.block, a.operation} < std::pair{b.block, b.operation};
    });
    return layout;
}
} // namespace moonstone::plaid::weaver
