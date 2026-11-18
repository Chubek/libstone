#pragma once
#include "blackstone.h"
#include "firestone-interop/match.hpp"
#include <libir/SimpleSSAGraph.hpp>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace blackstone {
struct Kernel {
    std::string name, ir, phase, source, documentation;
    std::vector<std::string> requirements, preserves;
};
enum class IntegerSemantics { Unknown, Modular };
struct RunResult {
    bool changed = false;
    std::set<std::string> invalidated;
};
struct PipelineResult : RunResult {
    std::size_t rounds = 0;
    bool converged = false;
};

class Runtime {
public:
    Runtime();
    void load_file(const std::string& path);
    void load_directory(const std::string& path);
    std::vector<Kernel> kernels() const;
    /* Supplies the supported SSA analyses. Errors leave the graph untouched. */
    RunResult run(const std::string& name, firestone::ir::SimpleSSAGraph& graph,
                  IntegerSemantics semantics = IntegerSemantics::Unknown);
    /* Run the named kernels in order, until a round has no changes or the
     * positive round limit is reached. Commit the whole pipeline on success,
     * even at the limit; errors leave the original graph untouched. */
    PipelineResult run_pipeline(const std::vector<std::string>& names,
                                firestone::ir::SimpleSSAGraph& graph,
                                IntegerSemantics semantics = IntegerSemantics::Unknown,
                                std::size_t max_rounds = 1);
    blackstone_runtime* native_handle() const { return runtime_.get(); }
private:
    std::unique_ptr<blackstone_runtime, decltype(&blackstone_destroy)> runtime_;
};
} // namespace blackstone
