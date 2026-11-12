#include "LuaProfiler.hpp"
#include "Internal.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
// Lua 5.5 adds an explicit hash seed. Kaguya's allocator constructors still
// spell the two-argument API, even when not instantiated. Keep this adaptation
// local to the bridge, without changing either vendored library.
#if LUA_VERSION_NUM >= 505
inline lua_State *prolific_newstate(lua_Alloc alloc, void *user) {
    return lua_newstate(alloc, user, 0);
}
// Lua 5.5 replaced the old GC setters and changed GCSTEP's vararg type/units.
// Adapt the real operations instead of aliasing constants with wrong semantics.
#define LUA_GCSETPAUSE (-10001)
#define LUA_GCSETSTEPMUL (-10002)
inline int prolific_gc(lua_State *state, int operation, int value) {
    if (operation == LUA_GCSETPAUSE)
        return lua_gc(state, LUA_GCPARAM, LUA_GCPPAUSE, value);
    if (operation == LUA_GCSETSTEPMUL)
        return lua_gc(state, LUA_GCPARAM, LUA_GCPSTEPMUL, value);
    if (operation == LUA_GCSTEP) {
        auto bytes = value > 0 ? static_cast<std::size_t>(value) : 0;
        bytes = bytes > SIZE_MAX / 1024 ? SIZE_MAX : bytes * 1024;
        return lua_gc(state, operation, bytes);
    }
    return lua_gc(state, operation);
}
#define lua_newstate(alloc, user) prolific_newstate(alloc, user)
#define lua_gc(state, operation, value) prolific_gc(state, operation, value)
#endif
#include <kaguya/kaguya.hpp>
#if LUA_VERSION_NUM >= 505
#undef lua_newstate
#undef lua_gc
#undef LUA_GCSETPAUSE
#undef LUA_GCSETSTEPMUL
#endif

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace moonstone::prolific {
static_assert(sizeof(lua_Integer) >= sizeof(std::int64_t),
              "Prolific Lua requires 64-bit integers for exact sample values");
namespace {
struct Budget {
    std::size_t remaining;
    bool exhausted = false;
};
void instruction_hook(lua_State *state, lua_Debug *) {
    auto *budget = *static_cast<Budget **>(lua_getextraspace(state));
    if (budget->remaining <= 100) {
        budget->exhausted = true;
        luaL_error(state, "Prolific Lua instruction budget exhausted");
    } else
        budget->remaining -= 100;
}
struct Callback {
    Domain domain;
    std::string kind;
    kaguya::LuaFunction function;
};
bool supported_kind(std::string_view kind) {
    if (kind == "*")
        return true;
    for (unsigned i = 0; i != 7; ++i)
        if (kind == name(static_cast<Kind>(i)))
            return true;
    return false;
}
kaguya::LuaTable event_table(kaguya::State &state, const Event &e) {
    auto table = state.newTable();
    // Decimal strings preserve exact 64-bit identities/times on every Lua build.
    table["sequence"] = std::to_string(e.sequence);
    table["timestamp_ns"] = std::to_string(e.timestamp_ns);
    table["thread"] = std::to_string(e.thread);
    table["span"] = std::to_string(e.span);
    table["parent"] = std::to_string(e.parent);
    table["duration_ns"] = std::to_string(e.duration_ns);
    table["self_ns"] = std::to_string(e.self_ns);
    table["domain"] = name(e.domain);
    table["kind"] = name(e.kind);
    table["name"] = e.name;
    table["detail"] = e.detail;
    table["value"] = e.value;
    auto loc = state.newTable();
    loc["file"] = e.location.file;
    loc["ir_id"] = e.location.ir_id;
    loc["line"] = e.location.line;
    loc["column"] = e.location.column;
    table["location"] = loc;
    return table;
}
} // namespace

Report analyze(Snapshot snapshot, std::string_view script, ScriptLimits limits) {
    Report report;
    report.data = std::move(snapshot);
    try {
        if (!limits.instructions || !limits.text || !limits.findings || !limits.source_bytes ||
            limits.findings > 100000 || limits.text > 1024 * 1024 ||
            script.size() > limits.source_bytes || !detail::valid_utf8(script))
            throw std::runtime_error("invalid or oversized Prolific Lua script/limits");
        std::string lua_error;
        Budget budget{limits.instructions};
        std::vector<Callback> callbacks;
        bool registering = true;
        bool api_exhausted = false;
        const Event *current = nullptr;
        std::optional<Domain> finishing;
        kaguya::State state(kaguya::LoadLibs{{"_G", luaopen_base},
                                             {"table", luaopen_table},
                                             {"string", luaopen_string},
                                             {"math", luaopen_math},
                                             {"utf8", luaopen_utf8}});
        auto *raw = state.state();
        if (!raw)
            throw std::runtime_error("cannot allocate Prolific Lua state");
        // Release Lua references before closing the VM, but keep callback
        // captures alive during Lua finalizers (including on exceptional exit).
        struct Cleanup {
            std::vector<Callback> &callbacks;
            bool &registering;
            const Event *&current;
            std::optional<Domain> &finishing;
            ~Cleanup() {
                registering = false;
                current = nullptr;
                finishing.reset();
                callbacks.clear();
            }
        } cleanup{callbacks, registering, current, finishing};
        state.setErrorHandler(
            [&](int, const char *text) { lua_error = text ? text : "Lua error"; });
        for (const auto *key : {"dofile", "loadfile", "load", "print", "collectgarbage"}) {
            lua_pushnil(raw);
            lua_setglobal(raw, key);
        }
        *static_cast<Budget **>(lua_getextraspace(raw)) = &budget;
        lua_sethook(raw, instruction_hook, LUA_MASKCOUNT, 100);
        auto module = [&](Domain domain) {
            auto table = state.newTable();
            table["on"] =
                kaguya::function([&, domain](const std::string &kind, kaguya::LuaFunction f) {
                    if (!registering || !supported_kind(kind) || f.type() != LUA_TFUNCTION)
                        throw std::runtime_error(
                            "invalid/late/excessive Prolific callback registration");
                    if (callbacks.size() >= 256) {
                        api_exhausted = true;
                        throw std::runtime_error("Prolific callback limit exhausted");
                    }
                    callbacks.push_back({domain, kind, std::move(f)});
                });
            table["finish"] = kaguya::function([&, domain](kaguya::LuaFunction f) {
                if (!registering || f.type() != LUA_TFUNCTION)
                    throw std::runtime_error("invalid/late/excessive Prolific finish callback");
                if (callbacks.size() >= 256) {
                    api_exhausted = true;
                    throw std::runtime_error("Prolific callback limit exhausted");
                }
                callbacks.push_back({domain, "finish", std::move(f)});
            });
            table["finding"] =
                kaguya::function([&, domain](const std::string &severity, const std::string &code,
                                             const std::string &message) {
                    if (((!current || current->domain != domain) && finishing != domain) ||
                        !detail::severity(severity) || code.empty() || message.empty() ||
                        !detail::valid_utf8(code) || !detail::valid_utf8(message))
                        throw std::runtime_error("invalid or exhausted Prolific finding");
                    if (code.size() > limits.text || message.size() > limits.text ||
                        report.findings.size() >= limits.findings) {
                        api_exhausted = true;
                        throw std::runtime_error("Prolific finding limit exhausted");
                    }
                    report.findings.push_back(
                        {severity, code, message, current ? current->sequence : 0});
                });
            table["version"] = 1;
            return table;
        };
        auto cc = module(Domain::Compiler), vm = module(Domain::VM);
        state["require"] = kaguya::function([cc, vm](const std::string &module_name) {
            if (module_name == "lprofcc")
                return cc;
            if (module_name == "lprofvm")
                return vm;
            throw std::runtime_error("unsupported Prolific Lua module: " + module_name);
        });
        if (luaL_loadbufferx(raw, script.data(), script.size(), "@prolific", "t") != LUA_OK ||
            lua_pcall(raw, 0, 0, 0) != LUA_OK) {
            const char *error = lua_tostring(raw, -1);
            throw std::runtime_error(error ? error : "Prolific Lua load/execute error");
        }
        if (!lua_error.empty())
            throw std::runtime_error(lua_error);
        if (api_exhausted)
            throw std::runtime_error("Prolific Lua API budget exhausted");
        registering = false;
        for (const auto &event : report.data.events) {
            current = &event;
            for (auto &cb : callbacks) {
                if (cb.domain != event.domain || (cb.kind != "*" && cb.kind != name(event.kind)))
                    continue;
                auto result = cb.function.call<kaguya::FunctionResults>(event_table(state, event));
                if (result.resultStatus() != LUA_OK || !lua_error.empty())
                    throw std::runtime_error(lua_error.empty() ? "Prolific Lua callback failed"
                                                               : lua_error);
                if (budget.exhausted || api_exhausted)
                    throw std::runtime_error("Prolific Lua budget exhausted");
            }
        }
        current = nullptr;
        for (auto &cb : callbacks) {
            if (cb.kind != "finish")
                continue;
            finishing = cb.domain;
            auto summary = state.newTable();
            summary["complete"] = report.data.complete();
            summary["rejected"] = std::to_string(report.data.rejected);
            summary["open_spans"] = std::to_string(report.data.open_spans);
            summary["event_count"] = std::to_string(report.data.events.size());
            auto result = cb.function.call<kaguya::FunctionResults>(summary);
            if (result.resultStatus() != LUA_OK || !lua_error.empty())
                throw std::runtime_error(lua_error.empty() ? "Prolific Lua finish failed"
                                                           : lua_error);
            if (budget.exhausted || api_exhausted)
                throw std::runtime_error("Prolific Lua budget exhausted");
            finishing.reset();
        }
        if (budget.exhausted || api_exhausted)
            throw std::runtime_error("Prolific Lua budget exhausted");
        report.analysis_status = "completed";
    } catch (const std::exception &error) {
        report.findings.clear();
        report.analysis_status = "failed";
        report.analysis_error = error.what();
        if (report.analysis_error.size() > 4096)
            report.analysis_error.resize(4096);
        if (report.analysis_error.empty() || !detail::valid_utf8(report.analysis_error))
            report.analysis_error = "Prolific Lua analysis failed";
    }
    return report;
}
} // namespace moonstone::prolific
