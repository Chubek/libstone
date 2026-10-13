#include "runtime.hpp"
#include <algorithm>
#include <filesystem>
#include <set>

namespace blackstone::lua {
std::string string_field(lua_State* L, int index, const char* name) {
    raw_field(L, index, name);
    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1); throw std::runtime_error(std::string("expected string field '") + name + "'");
    }
    size_t length = 0; const char* text = lua_tolstring(L, -1, &length);
    std::string result(text, length); lua_pop(L, 1);
    if (result.empty() || result.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("empty or invalid field '") + name + "'");
    return result;
}
std::vector<std::string> string_list(lua_State* L, int index, const char* name) {
    raw_field(L, index, name);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); throw std::runtime_error(std::string("expected list '") + name + "'"); }
    int table = lua_absindex(L, -1);
    std::vector<std::string> result; std::set<std::string> unique;
    auto length = lua_rawlen(L, table);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        bool valid = lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1 &&
                     static_cast<size_t>(lua_tointeger(L, -2)) <= length;
        lua_pop(L, 1);
        if (!valid) throw std::runtime_error(std::string("expected dense list '") + name + "'");
    }
    for (size_t i = 1; i <= length; ++i) {
        lua_rawgeti(L, table, static_cast<lua_Integer>(i));
        if (lua_type(L, -1) != LUA_TSTRING) throw std::runtime_error(std::string("expected string in '") + name + "'");
        size_t n; const char* p = lua_tolstring(L, -1, &n); std::string text(p, n); lua_pop(L, 1);
        if (text.empty() || text.find('\0') != std::string::npos || !unique.insert(text).second)
            throw std::runtime_error(std::string("empty, invalid or duplicate entry in '") + name + "'");
        result.push_back(std::move(text));
    }
    lua_pop(L, 1); return result;
}
}

void blackstone_runtime::load(const std::string& path) {
    using namespace blackstone::lua;
    StackGuard stack(state);
    try {
        lua_getglobal(state, "lrewrite"); raw_field(state, -1, "new_environment");
        checked_call(state, 0, 1);
        int environment = lua_absindex(state, -1);
        if (luaL_loadfilex(state, path.c_str(), "t") != LUA_OK) throw std::runtime_error(error_text(state));
        lua_pushvalue(state, environment); lua_setupvalue(state, -2, 1);
        checked_call(state, 0, 0);
        raw_field(state, environment, "lrewrite"); raw_field(state, -1, "kernel");
        int kernel = lua_absindex(state, -1);
        raw_field(state, kernel, "metadata");
        if (!lua_istable(state, -1)) throw std::runtime_error("expected one lrewrite.kernel.define declaration");
        int metadata = lua_absindex(state, -1);
        blackstone::Kernel info;
        info.name = string_field(state, metadata, "name");
        info.ir = string_field(state, metadata, "ir");
        info.phase = string_field(state, metadata, "phase");
        info.requirements = string_list(state, metadata, "requires");
        info.preserves = string_list(state, metadata, "preserves");
        raw_field(state, metadata, "documentation");
        if (!lua_isnil(state, -1)) { lua_pop(state, 1); info.documentation = string_field(state, metadata, "documentation"); }
        else lua_pop(state, 1);
        info.source = path;
        for (const auto& entry : entries) if (entry.info.name == info.name)
            throw std::runtime_error("duplicate kernel name: " + info.name);
        raw_field(state, kernel, "run");
        if (!lua_isfunction(state, -1)) throw std::runtime_error("expected lrewrite.kernel.run function");
        entries.push_back({std::move(info), LUA_NOREF});
        entries.back().run = luaL_ref(state, LUA_REGISTRYINDEX);
    } catch (const std::exception& e) { throw std::runtime_error(path + ": " + e.what()); }
}

extern "C" int blackstone_load_file(blackstone_runtime* r, const char* path) {
    if (!r) return -1;
    r->error.clear();
    try { if (!path) throw std::invalid_argument("null kernel path"); r->load(path); return 0; }
    catch (const std::exception& e) { r->error = e.what(); return -1; }
}
extern "C" int blackstone_load_directory(blackstone_runtime* r, const char* path) {
    if (!r) return -1;
    r->error.clear(); auto original = r->entries.size();
    try {
        if (!path || !std::filesystem::is_directory(path)) throw std::invalid_argument("kernel path is not a directory");
        std::vector<std::string> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
            if (entry.is_regular_file() && entry.path().extension() == ".lua") files.push_back(entry.path().string());
        std::sort(files.begin(), files.end());
        for (const auto& file : files) r->load(file);
        return 0;
    } catch (const std::exception& e) {
        while (r->entries.size() > original) {
            luaL_unref(r->state, LUA_REGISTRYINDEX, r->entries.back().run); r->entries.pop_back();
        }
        r->error = e.what(); return -1;
    }
}
