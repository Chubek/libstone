#include "runtime.hpp"
#include "../lrewrite/lrewrite.hpp"
#include <cstring>

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
}
extern "C" int blackstone_run(blackstone_runtime* r, const char* name, const char* ir,
                              const char* const* available, size_t count,
                              blackstone_push_context push, void* user, int* changed) {
    if (changed) *changed = 0;
    if (!r) return -1;
    r->error.clear(); blackstone::lua::StackGuard stack(r->state);
    try {
        if (!name || !ir || !push || !changed || (count && !available)) throw std::invalid_argument("invalid run arguments");
        const blackstone_runtime::Entry* kernel = nullptr;
        for (const auto& entry : r->entries) if (entry.info.name == name) kernel = &entry;
        if (!kernel) throw std::invalid_argument(std::string("unknown kernel: ") + name);
        if (kernel->info.ir != ir) throw std::invalid_argument("kernel IR does not match host IR");
        for (const auto& requirement : kernel->info.requirements) {
            bool found = false;
            for (size_t n = 0; n < count; ++n) if (available[n] && requirement == available[n]) found = true;
            if (!found) throw std::invalid_argument("missing required analysis: " + requirement);
        }
        Invocation invocation{push, user, kernel->run};
        lua_pushcfunction(r->state, invoke); lua_pushlightuserdata(r->state, &invocation);
        blackstone::lua::checked_call(r->state, 1, 1);
        *changed = lua_toboolean(r->state, -1); return 0;
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
}
RunResult Runtime::run(const std::string& name, firestone::ir::SimpleSSAGraph& graph, IntegerSemantics semantics) {
    auto errors = graph.verify();
    if (!errors.empty()) throw std::invalid_argument("invalid input SSA: " + errors.front().message);
    auto working = graph;
    SSAInvocation invocation{&working, semantics == IntegerSemantics::Modular};
    RegistryGuard reference{runtime_->state, invocation}; lua::StackGuard stack(runtime_->state);
    const char* analyses[] = {"loop_info", "def_use", "dominators", "cfg"};
    int changed = 0;
    if (blackstone_run(runtime_.get(), name.c_str(), "ssa", analyses, 4, push_context, &invocation, &changed))
        throw std::runtime_error(blackstone_last_error(runtime_.get()));
    RunResult result; result.changed = changed != 0;
    if (!result.changed) return result;
    lua_rawgeti(runtime_->state, LUA_REGISTRYINDEX, invocation.context);
    lua::read_ssa_context(runtime_->state, -1, &working);
    errors = working.verify();
    if (!errors.empty()) throw std::runtime_error("invalid rewritten SSA: " + errors.front().message);
    for (const auto& entry : runtime_->entries) if (entry.info.name == name)
        for (const auto* analysis : analyses)
            if (std::find(entry.info.preserves.begin(), entry.info.preserves.end(), analysis) == entry.info.preserves.end())
                result.invalidated.insert(analysis);
    graph = std::move(working); return result;
}
} // namespace blackstone
