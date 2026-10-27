#pragma once
#include "TraceSched.hpp"
namespace firestone::isched {
struct SuperblockSched {
    std::vector<ir::BlockId> region(const ir::ControlFlowGraph& cfg, ir::BlockId entry,
                                  const EdgeFrequencies& weights = {}) const { return detail::hot_path(cfg, entry, weights, true); }
    RegionResult schedule(const Graph& g, const ir::ControlFlowGraph& cfg,
                          const std::vector<ir::BlockId>& blocks, const Machine& m = {}) const {
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            cfg.block(blocks[i]);
            if (i && cfg.predecessors(blocks[i]) != std::vector<ir::BlockId>{blocks[i - 1]})
                throw std::invalid_argument("superblock must be a single-entry path");
        }
        RegionResult out{blocks, detail::region_graph(g, blocks, true), {}};
        auto speculative = Speculative{}.schedule(out.graph, m);
        out.graph = std::move(speculative.graph); out.schedule = std::move(speculative.schedule); return out;
    }
};
} // namespace firestone::isched
