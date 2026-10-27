#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
struct Copy { Location from, to; };
struct ParallelCopyResolution {
    // Locations are atomic, nonaliasing storage cells. Scratch must be distinct
    // from every source/destination; it can be a dedicated temporary stack slot.
    std::vector<Copy> resolve(std::vector<Copy> copies, Location scratch) const {
        auto storage = [](Location l) { return l.kind == LocationKind::Register || l.kind == LocationKind::Stack; };
        if (!storage(scratch)) throw std::invalid_argument("invalid parallel-copy scratch");
        for (std::size_t i = 0; i < copies.size(); ++i) {
            const auto& c = copies[i];
            if (!storage(c.from) || !storage(c.to) || c.from == scratch || c.to == scratch)
                throw std::invalid_argument("invalid parallel copy");
            for (std::size_t j = 0; j < i; ++j) if (copies[j].to == c.to) throw std::invalid_argument("duplicate parallel-copy destination");
        }
        copies.erase(std::remove_if(copies.begin(), copies.end(), [](const auto& c) { return c.from == c.to; }), copies.end());
        std::vector<Copy> out;
        while (!copies.empty()) {
            auto safe = copies.end();
            for (auto it = copies.begin(); it != copies.end(); ++it) {
                bool needed = false;
                for (auto c : copies) if (c.from == it->to) needed = true;
                if (!needed) { safe = it; break; }
            }
            if (safe != copies.end()) { out.push_back(*safe); copies.erase(safe); }
            else {
                auto saved = copies.front().to; out.push_back({saved, scratch});
                for (auto& c : copies) if (c.from == saved) c.from = scratch;
            }
        }
        return out;
    }
};
} // namespace firestone::regalloc
