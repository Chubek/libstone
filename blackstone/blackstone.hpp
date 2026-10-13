#pragma once
#include "blackstone.h"
#include "firestone-interop/match.hpp"
#include <libir/SimpleSSAGraph.hpp>
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

class Runtime {
public:
    Runtime();
    void load_file(const std::string& path);
    void load_directory(const std::string& path);
    std::vector<Kernel> kernels() const;
    /* Computes the declared SSA analyses. Unknown arithmetic semantics never
     * authorize strength reduction. Errors leave the caller's graph untouched. */
    RunResult run(const std::string& name, firestone::ir::SimpleSSAGraph& graph,
                  IntegerSemantics semantics = IntegerSemantics::Unknown);
    blackstone_runtime* native_handle() const { return runtime_.get(); }
private:
    std::unique_ptr<blackstone_runtime, decltype(&blackstone_destroy)> runtime_;
};
} // namespace blackstone
