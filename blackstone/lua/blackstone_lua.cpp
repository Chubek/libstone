#include "runtime.hpp"
#include "../lrewrite/lrewrite.hpp"
#include <algorithm>
#include <iterator>
#include <string_view>

extern "C" {
blackstone_runtime* blackstone_create(void) {
    try { return new blackstone_runtime; } catch (...) { return nullptr; }
}
void blackstone_destroy(blackstone_runtime* r) { delete r; }
const char* blackstone_last_error(const blackstone_runtime* r) { return r ? r->error.c_str() : "null Blackstone runtime"; }
size_t blackstone_kernel_count(const blackstone_runtime* r) { return r ? r->entries.size() : 0; }
int blackstone_kernel_get(const blackstone_runtime* r, size_t index, blackstone_kernel_info* out) {
    if (!r || !out || index >= r->entries.size()) return -1;
    const auto& k = r->entries[index].info;
    *out = {k.name.c_str(), k.ir.c_str(), k.phase.c_str(), k.source.c_str(), k.documentation.c_str()}; return 0;
}
size_t blackstone_requirement_count(const blackstone_runtime* r, size_t i) {
    return r && i < r->entries.size() ? r->entries[i].info.requirements.size() : 0;
}
const char* blackstone_requirement_get(const blackstone_runtime* r, size_t i, size_t n) {
    return n < blackstone_requirement_count(r, i) ? r->entries[i].info.requirements[n].c_str() : nullptr;
}
size_t blackstone_preservation_count(const blackstone_runtime* r, size_t i) {
    return r && i < r->entries.size() ? r->entries[i].info.preserves.size() : 0;
}
const char* blackstone_preservation_get(const blackstone_runtime* r, size_t i, size_t n) {
    return n < blackstone_preservation_count(r, i) ? r->entries[i].info.preserves[n].c_str() : nullptr;
}
}

namespace {
struct Invocation { blackstone_push_context push; void* user; int run; };
int invoke(lua_State* L) {
    auto* invocation = static_cast<Invocation*>(lua_touserdata(L, 1));
    int top = lua_gettop(L);
    int count = invocation->push(L, invocation->user);
    if (count != 1 || lua_gettop(L) != top + 1 || !lua_istable(L, -1))
        return luaL_error(L, "host must push exactly one context table");
    lua_rawgeti(L, LUA_REGISTRYINDEX, invocation->run); lua_insert(L, -2);
    lua_call(L, 1, 1);
    if (!lua_isboolean(L, -1)) return luaL_error(L, "kernel.run must return a boolean");
    return 1;
}

const blackstone_runtime::Entry& resolve_kernel(const blackstone_runtime& r, std::string_view name,
                                                const char* ir, const char* const* available,
                                                size_t count) {
    auto kernel = std::find_if(r.entries.begin(), r.entries.end(),
                               [&](const auto& entry) { return entry.info.name == name; });
    if (kernel == r.entries.end()) throw std::invalid_argument("unknown kernel: " + std::string(name));
    if (kernel->info.ir != ir) throw std::invalid_argument("kernel IR does not match host IR");
    for (const auto& requirement : kernel->info.requirements) {
        bool found = false;
        for (size_t n = 0; n < count; ++n)
            if (available[n] && requirement == available[n]) { found = true; break; }
        if (!found) throw std::invalid_argument("missing required analysis: " + requirement);
    }
    return *kernel;
}

bool invoke_kernel(lua_State* state, const blackstone_runtime::Entry& kernel,
                   blackstone_push_context push, void* user) {
    blackstone::lua::StackGuard stack(state);
    Invocation invocation{push, user, kernel.run};
    lua_pushcfunction(state, invoke);
    lua_pushlightuserdata(state, &invocation);
    blackstone::lua::checked_call(state, 1, 1);
    return lua_toboolean(state, -1) != 0;
}
}
extern "C" int blackstone_run(blackstone_runtime* r, const char* name, const char* ir,
                              const char* const* available, size_t count,
                              blackstone_push_context push, void* user, int* changed) {
    if (changed) *changed = 0;
    if (!r) return -1;
    r->error.clear();
    try {
        if (!name || !ir || !push || !changed || (count && !available)) throw std::invalid_argument("invalid run arguments");
        const auto& kernel = resolve_kernel(*r, name, ir, available, count);
        *changed = invoke_kernel(r->state, kernel, push, user);
        return 0;
    } catch (const std::exception& e) { r->error = std::string(name ? name : "<null>") + ": " + e.what(); return -1; }
}

namespace blackstone {
Runtime::Runtime() : runtime_(blackstone_create(), blackstone_destroy) {
    if (!runtime_) throw std::runtime_error("could not create Blackstone Lua runtime");
}
void Runtime::load_file(const std::string& path) {
    if (blackstone_load_file(runtime_.get(), path.c_str())) throw std::runtime_error(blackstone_last_error(runtime_.get()));
}
void Runtime::load_directory(const std::string& path) {
    if (blackstone_load_directory(runtime_.get(), path.c_str())) throw std::runtime_error(blackstone_last_error(runtime_.get()));
}
std::vector<Kernel> Runtime::kernels() const {
    std::vector<Kernel> result;
    for (const auto& entry : runtime_->entries) result.push_back(entry.info);
    return result;
}
namespace {
using firestone::ir::SimpleSSAGraph;
constexpr const char* ssa_analyses[] = {"loop_info", "def_use", "dominators", "cfg"};

const blackstone_runtime::Entry& resolve_ssa_kernel(const blackstone_runtime& r, const std::string& name) {
    try {
        return resolve_kernel(r, name, "ssa", ssa_analyses, std::size(ssa_analyses));
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument(name + ": " + e.what());
    }
}

struct SSAInvocation { const firestone::ir::SimpleSSAGraph* graph; bool modular; int context = LUA_NOREF; };
int push_context(lua_State* L, void* raw) {
    auto& invocation = *static_cast<SSAInvocation*>(raw);
    bool failed = false;
    try {
        lua::push_ssa_context(L, invocation.graph, invocation.modular);
        lua_pushvalue(L, -1); invocation.context = luaL_ref(L, LUA_REGISTRYINDEX);
    } catch (const std::exception& e) { lua_pushstring(L, e.what()); failed = true; }
    if (failed) return lua_error(L);
    return 1;
}
struct RegistryGuard {
    lua_State* state; SSAInvocation& invocation;
    ~RegistryGuard() { luaL_unref(state, LUA_REGISTRYINDEX, invocation.context); }
};

void verify_input(const SimpleSSAGraph& graph) {
    const auto errors = graph.verify();
    if (!errors.empty()) throw std::invalid_argument("invalid input SSA: " + errors.front().message);
}

// A pipeline owns a disposable working copy. A single-kernel call can delay
// copying the graph until the Lua kernel reports a change.
RunResult rewrite_ssa(lua_State* state, const blackstone_runtime::Entry& kernel,
                      SimpleSSAGraph& graph, IntegerSemantics semantics, bool disposable) {
    SSAInvocation invocation{&graph, semantics == IntegerSemantics::Modular};
    RegistryGuard reference{state, invocation};
    lua::StackGuard stack(state);
    if (!invoke_kernel(state, kernel, push_context, &invocation)) return {};

    SimpleSSAGraph copy;
    if (!disposable) copy = graph;
    auto& working = disposable ? graph : copy;
    lua_rawgeti(state, LUA_REGISTRYINDEX, invocation.context);
    lua::read_ssa_context(state, -1, &working);
    const auto errors = working.verify();
    if (!errors.empty()) throw std::runtime_error("invalid rewritten SSA: " + errors.front().message);
    RunResult result;
    result.changed = true;
    const auto& preserves = kernel.info.preserves;
    for (const auto* analysis : ssa_analyses)
        if (std::find(preserves.begin(), preserves.end(), analysis) == preserves.end())
            result.invalidated.insert(analysis);
    if (!disposable) graph = std::move(copy);
    return result;
}
}

RunResult Runtime::run(const std::string& name, SimpleSSAGraph& graph, IntegerSemantics semantics) {
    verify_input(graph);
    runtime_->error.clear();
    try {
        const auto& kernel = resolve_kernel(*runtime_, name, "ssa", ssa_analyses, std::size(ssa_analyses));
        return rewrite_ssa(runtime_->state, kernel, graph, semantics, false);
    } catch (const std::exception& e) {
        runtime_->error = name + ": " + e.what();
        throw std::runtime_error(runtime_->error);
    }
}

PipelineResult Runtime::run_pipeline(const std::vector<std::string>& names, SimpleSSAGraph& graph,
                                     IntegerSemantics semantics, std::size_t max_rounds) {
    if (!max_rounds) throw std::invalid_argument("pipeline round limit must be positive");
    verify_input(graph);
    PipelineResult result;
    if (names.empty()) {
        result.converged = true;
        return result;
    }

    std::vector<const blackstone_runtime::Entry*> kernels;
    kernels.reserve(names.size());
    for (const auto& name : names) kernels.push_back(&resolve_ssa_kernel(*runtime_, name));

    auto working = graph;
    while (result.rounds < max_rounds) {
        ++result.rounds;
        bool changed = false;
        for (const auto* kernel : kernels) {
            try {
                auto step = rewrite_ssa(runtime_->state, *kernel, working, semantics, true);
                changed |= step.changed;
                result.invalidated.merge(step.invalidated);
            } catch (const std::exception& e) {
                throw std::runtime_error("pipeline round " + std::to_string(result.rounds) +
                                         ", kernel '" + kernel->info.name + "': " + e.what());
            }
        }
        if (!changed) {
            result.converged = true;
            break;
        }
        result.changed = true;
    }
    if (result.changed) graph = std::move(working);
    return result;
}
} // namespace blackstone
