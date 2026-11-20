#include "lua_runtime.hpp"
#include <lua.hpp>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace domios {
struct LiosRuntime::Impl {
    bool ready{false};
};
namespace {
struct Budget { std::size_t bytes{}; };
struct Invocation { const std::string& code; const std::vector<std::string>& args; };
void* allocate(void* user, void* ptr, std::size_t old_size, std::size_t new_size) {
    auto& budget = *static_cast<Budget*>(user);
    if (!ptr) old_size = 0;
    if (!new_size) { budget.bytes -= old_size; std::free(ptr); return nullptr; }
    if (new_size > 8 * 1024 * 1024 - (budget.bytes - old_size)) return nullptr;
    auto* result = std::realloc(ptr, new_size);
    if (result) budget.bytes = budget.bytes - old_size + new_size;
    return result;
}
void limit(lua_State* state, lua_Debug*) { luaL_error(state, "Lua instruction budget exceeded"); }
int run(lua_State* state) {
    // All setup runs inside a protected call, including allocation failures.
    const auto& invocation = *static_cast<const Invocation*>(lua_touserdata(state, 1));
    for (const auto& lib : {std::pair{LUA_GNAME, luaopen_base},
                           std::pair{LUA_TABLIBNAME, luaopen_table},
                           std::pair{LUA_STRLIBNAME, luaopen_string},
                           std::pair{LUA_MATHLIBNAME, luaopen_math},
                           std::pair{LUA_UTF8LIBNAME, luaopen_utf8}}) {
        luaL_requiref(state, lib.first, lib.second, 1); lua_pop(state, 1);
    }
    for (auto name : {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall", "print"}) {
        lua_pushnil(state); lua_setglobal(state, name);
    }
    lua_createtable(state, 0, 0);
    for (std::size_t i = 0; i < invocation.args.size(); ++i) {
        lua_pushlstring(state, invocation.args[i].data(), invocation.args[i].size());
        lua_rawseti(state, -2, i + 1);
    }
    lua_setglobal(state, "arg");
    if (luaL_loadbufferx(state, invocation.code.data(), invocation.code.size(), "lios", "t") != LUA_OK)
        return lua_error(state);
    lua_sethook(state, limit, LUA_MASKCOUNT, 1000000);
    lua_call(state, 0, 1);
    if (lua_isnumber(state, -1)) lua_tolstring(state, -1, nullptr);
    return 1;
}
}

LiosRuntime::LiosRuntime() : impl_(std::make_unique<Impl>()) {}
LiosRuntime::~LiosRuntime() { shutdown(); }

bool LiosRuntime::initialize(const Config& config, EventQueue& events, std::string& error) {
    impl_->ready = true;
    if (!load_disk_programs(config.extra_program_dirs, events)) {
        error = "Some Lua programs could not be loaded";
        return false;
    }
    return true;
}
bool LiosRuntime::load_embedded_program(const std::string& name, const std::string& code) {
    if (name.empty() || code.size() > 1024 * 1024 || code.find('\0') != std::string::npos) return false;
    programs_[name] = code;
    return true;
}
bool LiosRuntime::load_disk_programs(const std::vector<std::filesystem::path>& dirs, EventQueue& events) {
    try {
        for (const auto& dir : dirs) {
            if (!std::filesystem::exists(dir)) continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".lua") continue;
                if (entry.file_size() > 1024 * 1024) return false;
                std::ifstream file(entry.path());
                std::ostringstream code; code << file.rdbuf();
                if (!file || !load_embedded_program(entry.path().stem().string(), code.str())) return false;
            }
        }
        return true;
    } catch (const std::exception& e) {
        events.try_push({EventKind::diagnostic, 1, "lua", e.what()}); return false;
    }
}
bool LiosRuntime::has_program(const std::string& name) const { return programs_.contains(name); }

std::string LiosRuntime::run_program(const std::string& name, const std::vector<std::string>& args,
                                    EventQueue& events, std::string* error) {
    if (error) error->clear();
    const auto it = programs_.find(name);
    if (!impl_->ready || it == programs_.end()) {
        if (error) *error = "Lua runtime or program not available";
        return {};
    }
    Budget budget;
    auto* state = lua_newstate(allocate, &budget);
    if (!state) { if (error) *error = "Lua allocation failed"; return "Lua allocation failed"; }
    const Invocation invocation{it->second, args};
    lua_pushcfunction(state, run);
    lua_pushlightuserdata(state, const_cast<Invocation*>(&invocation));
    const int status = lua_pcall(state, 1, 1, 0);
    std::size_t size = 0;
    const char* text = lua_tolstring(state, -1, &size);
    std::string result = text ? std::string(text, size) : lua_isboolean(state, -1) ? (lua_toboolean(state, -1) ? "true" : "false") : "";
    if (status != LUA_OK) {
        if (result.empty()) result = "Lua execution failed";
        events.try_push({EventKind::diagnostic, 1, name, result});
        if (error) *error = result;
    }
    lua_close(state);
    return result;
}
bool LiosRuntime::sandbox_intact() const { return impl_->ready; }
void LiosRuntime::shutdown() { programs_.clear(); impl_->ready = false; }
} // namespace domios
