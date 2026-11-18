#pragma once
#include "Common.hpp"

namespace firestone::regalloc {
struct Copy { Location from, to; };
struct ParallelCopyResolution {
    // Locations are atomic, nonaliasing storage cells. Scratch must be distinct
    // from every source/destination; it can be a dedicated temporary stack slot.
    std::vector<Copy> resolve(std::vector<Copy> copies, Location scratch) const {
        auto storage = [](Location l) {
            return l.index != invalid_id && (l.kind == LocationKind::Register || l.kind == LocationKind::Stack);
        };
        auto key = [](Location l) { return std::make_pair(l.kind, l.index); };
        using Key = decltype(key(scratch));
        std::map<Key, std::size_t> destination;
        if (!storage(scratch)) throw std::invalid_argument("invalid parallel-copy scratch");
        for (std::size_t i = 0; i < copies.size(); ++i) {
            const auto& c = copies[i];
            if (!storage(c.from) || !storage(c.to) || c.from == scratch || c.to == scratch)
                throw std::invalid_argument("invalid parallel copy");
            if (!destination.emplace(key(c.to), i).second) throw std::invalid_argument("duplicate parallel-copy destination");
        }
        std::map<Key, std::set<std::size_t>> readers;
        std::set<std::size_t> pending, ready;
        for (std::size_t i = 0; i < copies.size(); ++i) if (copies[i].from != copies[i].to) {
            pending.insert(i);
            readers[key(copies[i].from)].insert(i);
        }
        for (auto i : pending) if (!readers.count(key(copies[i].to))) ready.insert(i);
        std::vector<Copy> out;
        while (!pending.empty()) {
            if (ready.empty()) {
                // Every pending destination is still read, so the remaining
                // moves form cycles. Scratch has no readers between cycles.
                auto i = *pending.begin();
                auto saved = copies[i].to;
                out.push_back({saved, scratch});
                auto saved_readers = readers.extract(key(saved));
                for (auto reader : saved_readers.mapped()) copies[reader].from = scratch;
                saved_readers.key() = key(scratch);
                readers.insert(std::move(saved_readers));
                ready.insert(i);
            }
            auto i = *ready.begin(); ready.erase(ready.begin()); pending.erase(i);
            auto copy = copies[i]; out.push_back(copy);
            auto source = readers.find(key(copy.from));
            source->second.erase(i);
            if (!source->second.empty()) continue;
            readers.erase(source);
            auto writer = destination.find(key(copy.from));
            if (writer != destination.end() && pending.count(writer->second)) ready.insert(writer->second);
        }
        return out;
    }
};
} // namespace firestone::regalloc
