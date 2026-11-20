// A typed, private optimization hook. It can request native passes, not run
// the layout SDK, read input bytes, or change symbol/relocation semantics.
#include "Backend.hpp"
#include "RestrictedLua.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace qld::detail {
namespace {

constexpr size_t maxRoots = 128;
constexpr size_t maxSymbolName = 255;

struct Hook {
  const LinkState& state;
  size_t objectCount;
  bool gcSections = false;
  size_t rootCount = 0;
  std::array<std::array<char, maxSymbolName>, maxRoots> names{};
  std::array<size_t, maxRoots> sizes{};
};

// Lua errors can longjmp through these callbacks. Keep locals trivial and
// perform all operations that can raise Lua errors under a protected call.
int initialize(lua_State* lua) {
  restrictedLua::installGlobals(lua);
  return 0;
}

int readOnly(lua_State* lua) {
  return luaL_error(lua, "link overview is read-only");
}

void pushOverview(lua_State* lua, const Hook& hook) {
  const auto& state = hook.state;
  lua_createtable(lua, 0, 7);
  lua_pushliteral(lua, "elf");
  lua_setfield(lua, -2, "format");
  lua_pushliteral(lua, "static-exec");
  lua_setfield(lua, -2, "mode");
  const auto arch = qbfd::toString(state.arch);
  lua_pushlstring(lua, arch.data(), arch.size());
  lua_setfield(lua, -2, "architecture");
  lua_pushstring(lua, state.options.entry.empty() ? "_start" : state.options.entry.c_str());
  lua_setfield(lua, -2, "entry");
  lua_pushinteger(lua, state.wide ? 64 : 32);
  lua_setfield(lua, -2, "elf_class");
  lua_pushinteger(lua, static_cast<lua_Integer>(hook.objectCount));
  lua_setfield(lua, -2, "object_count");
  lua_pushinteger(lua, static_cast<lua_Integer>(state.defs.size()));
  lua_setfield(lua, -2, "symbol_count");

  // Use an empty proxy: __newindex on a populated table wouldn't protect
  // writes to existing fields.
  lua_newtable(lua);
  lua_createtable(lua, 0, 3);
  lua_pushvalue(lua, -3);
  lua_setfield(lua, -2, "__index");
  lua_pushcfunction(lua, readOnly);
  lua_setfield(lua, -2, "__newindex");
  lua_pushboolean(lua, false);
  lua_setfield(lua, -2, "__metatable");
  lua_setmetatable(lua, -2);
  lua_remove(lua, -2); // remove the backing table, leave only the proxy
}

void readRoots(lua_State* lua, Hook& hook) {
  const int table = lua_absindex(lua, -1);
  if (lua_type(lua, table) != LUA_TTABLE || lua_getmetatable(lua, table))
    luaL_error(lua, "optimize keep must be an array of symbol names");
  size_t last = 0;
  lua_pushnil(lua);
  while (lua_next(lua, table)) {
    if (!lua_isinteger(lua, -2) || lua_type(lua, -1) != LUA_TSTRING)
      luaL_error(lua, "optimize keep must be a dense array of symbol names");
    const lua_Integer key = lua_tointeger(lua, -2);
    if (key < 1 || key > static_cast<lua_Integer>(maxRoots))
      luaL_error(lua, "optimize keep index outside 1..%d", static_cast<int>(maxRoots));
    const size_t index = static_cast<size_t>(key);
    size_t size = 0;
    const char* name = lua_tolstring(lua, -1, &size);
    if (!size || size > maxSymbolName || std::memchr(name, '\0', size))
      luaL_error(lua, "optimize keep symbol must be 1..%d bytes without NUL",
                 static_cast<int>(maxSymbolName));
    std::memcpy(hook.names[index - 1].data(), name, size);
    hook.sizes[index - 1] = size;
    if (last < index) last = index;
    ++hook.rootCount;
    lua_pop(lua, 1);
  }
  if (last != hook.rootCount)
    luaL_error(lua, "optimize keep must be a dense array of symbol names");
}

int invoke(lua_State* lua) {
  auto& hook = *static_cast<Hook*>(lua_touserdata(lua, 1));
  lua_getglobal(lua, "optimize");
  if (lua_type(lua, -1) != LUA_TFUNCTION)
    return luaL_error(lua, "expected function optimize(link)");
  pushOverview(lua, hook);
  lua_call(lua, 1, LUA_MULTRET);
  if (lua_gettop(lua) != 2 || lua_type(lua, 2) != LUA_TTABLE ||
      lua_getmetatable(lua, 2))
    return luaL_error(lua, "optimize(link) must return exactly one table");

  bool hasGc = false;
  lua_pushnil(lua);
  while (lua_next(lua, 2)) {
    if (lua_type(lua, -2) != LUA_TSTRING)
      return luaL_error(lua, "unknown optimize result field");
    size_t size = 0;
    const char* key = lua_tolstring(lua, -2, &size);
    const std::string_view field(key, size);
    if (field == "gc_sections") {
      if (lua_type(lua, -1) != LUA_TBOOLEAN)
        return luaL_error(lua, "optimize result gc_sections must be a boolean");
      hasGc = true;
      hook.gcSections = lua_toboolean(lua, -1);
    } else if (field == "keep") {
      readRoots(lua, hook);
    } else {
      return luaL_error(lua, "unknown optimize result field");
    }
    lua_pop(lua, 1);
  }
  if (!hasGc)
    return luaL_error(lua, "optimize result requires boolean gc_sections");
  if (!hook.gcSections && hook.rootCount)
    return luaL_error(lua, "nonempty optimize keep requires gc_sections = true");
  return 0;
}

} // namespace

bool runLtoScript(LinkState& state, std::string& error) try {
  const auto& path = state.options.ltoScript;
  const auto source = toolchain::readText(path, restrictedLua::maxSource);
  size_t objects = 0;
  for (const auto& object : state.objects)
    if (object.object) ++objects;
  Hook hook{state, objects};
  restrictedLua::Budget budget{.name = "LTO script"};
  // The script is configured build input; use a stable string-hash seed.
  auto lua = restrictedLua::create(budget, 0);
  if (!lua) throw std::runtime_error("cannot allocate Lua state");

  lua_pushcfunction(lua.get(), initialize);
  int status = lua_pcall(lua.get(), 0, 0, 0);
  if (status == LUA_OK) {
    status = luaL_loadbufferx(lua.get(), source.data(), source.size(), "=LTO script", "t");
    if (status == LUA_OK) {
      lua_sethook(lua.get(), restrictedLua::countInstructions, LUA_MASKCOUNT,
                  restrictedLua::hookInterval);
      status = lua_pcall(lua.get(), 0, 0, 0);
    }
    if (status == LUA_OK) {
      lua_pushcfunction(lua.get(), invoke);
      lua_pushlightuserdata(lua.get(), &hook);
      status = lua_pcall(lua.get(), 1, 0, 0);
    }
  }
  if (status != LUA_OK) {
    if (status == LUA_ERRMEM)
      throw std::runtime_error(budget.allocationLimitHit ? "Lua allocation limit exceeded"
                                                       : "Lua out of memory");
    if (budget.instructions >= restrictedLua::maxInstructions)
      throw std::runtime_error("Lua instruction limit exceeded");
    if (lua_type(lua.get(), -1) == LUA_TSTRING) {
      size_t length = 0;
      const char* message = lua_tolstring(lua.get(), -1, &length);
      std::string_view text(message, std::min<size_t>(length, 1024));
      if (!text.empty() && text.front())
        throw std::runtime_error(std::string(text.substr(0, text.find('\0'))));
    }
    throw std::runtime_error("Lua error");
  }

  LtoPlan plan;
  plan.gcSections = hook.gcSections;
  for (size_t i = 0; i < hook.rootCount; ++i) {
    std::string name(hook.names[i].data(), hook.sizes[i]);
    if (std::find(plan.keepSymbols.begin(), plan.keepSymbols.end(), name) != plan.keepSymbols.end())
      throw std::runtime_error("duplicate optimize keep symbol: " + name);
    plan.keepSymbols.push_back(std::move(name));
  }
  state.ltoPlan = std::move(plan);
  return true;
} catch (const std::exception& e) {
  error = "--lto-script " + state.options.ltoScript + ": " + e.what();
  return false;
}

} // namespace qld::detail
