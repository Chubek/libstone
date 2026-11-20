// A separate, restricted Lua environment for portable registration records.
// Do not use the unrestricted layout-script runtime or invoke a native linker.
#include "ld.h"
#include "RestrictedLua.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {
namespace restricted = qld::detail::restrictedLua;
constexpr size_t maxRecords = 128;
constexpr size_t maxName = 64;
constexpr size_t maxRegistrar = 128;

struct Manifest {
  size_t count = 0;
  size_t size = 0;
  std::array<char, maxRecords * (maxName + maxRegistrar + 2)> bytes;
  std::array<std::string_view, maxRecords> names;
  std::array<std::string_view, maxRecords> registrars;
};

Manifest &context(lua_State *lua) {
  return *static_cast<Manifest *>(lua_touserdata(lua, lua_upvalueindex(1)));
}

bool lower(char c) { return c >= 'a' && c <= 'z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool cFirst(char c) { return lower(c) || (c >= 'A' && c <= 'Z') || c == '_'; }

// Lua errors can longjmp through this function, so keep its locals trivial.
int addBackend(lua_State *lua) {
  if (lua_gettop(lua) != 2 || lua_type(lua, 1) != LUA_TSTRING ||
      lua_type(lua, 2) != LUA_TSTRING)
    return luaL_error(lua, "qld.backend expects exactly two strings");
  size_t nameSize = 0, registrarSize = 0;
  const char *nameText = lua_tolstring(lua, 1, &nameSize);
  const char *registrarText = lua_tolstring(lua, 2, &registrarSize);
  const std::string_view name(nameText, nameSize), registrar(registrarText, registrarSize);
  if (name.size() > maxName)
    return luaL_error(lua, "backend name length limit exceeded");
  if (registrar.size() > maxRegistrar)
    return luaL_error(lua, "backend registrar length limit exceeded");
  if (name.empty() || !lower(name.front()))
    return luaL_error(lua, "invalid backend name");
  for (char c : name)
    if (!lower(c) && !digit(c) && c != '-')
      return luaL_error(lua, "invalid backend name");
  if (registrar.empty() || !cFirst(registrar.front()))
    return luaL_error(lua, "invalid backend registrar");
  for (char c : registrar)
    if (!cFirst(c) && !digit(c))
      return luaL_error(lua, "invalid backend registrar");

  auto &manifest = context(lua);
  if (manifest.count == maxRecords)
    return luaL_error(lua, "backend manifest registration limit exceeded");
  for (size_t i = 0; i < manifest.count; ++i) {
    if (manifest.names[i] == name)
      return luaL_error(lua, "duplicate backend name");
    if (manifest.registrars[i] == registrar)
      return luaL_error(lua, "duplicate backend registrar");
  }
  char *p = manifest.bytes.data() + manifest.size;
  std::memcpy(p, name.data(), name.size());
  manifest.names[manifest.count] = {p, name.size()};
  p += name.size();
  *p++ = '\t';
  std::memcpy(p, registrar.data(), registrar.size());
  manifest.registrars[manifest.count] = {p, registrar.size()};
  p += registrar.size();
  *p = '\n';
  manifest.size += name.size() + registrar.size() + 2;
  ++manifest.count;
  return 0;
}

int initialize(lua_State *lua) {
  // Install only these base functions in a fresh global environment. No
  // libraries are reachable, including load/loadfile, package, io, os,
  // coroutine, debug, or pcall (which could catch quota/validation errors).
  restricted::installGlobals(lua);
  lua_newtable(lua);
  lua_pushvalue(lua, 1);
  lua_pushcclosure(lua, addBackend, 1);
  lua_setfield(lua, -2, "backend");
  lua_setfield(lua, -2, "qld");
  return 0;
}

int fail(char **error, int status, std::string_view message) {
  if (error) {
    message = message.substr(0, 1024);
    message = message.substr(0, message.find('\0'));
    auto *copy = static_cast<char *>(std::malloc(message.size() + 1));
    if (copy) {
      std::memcpy(copy, message.data(), message.size());
      copy[message.size()] = '\0';
      *error = copy;
    }
  }
  return status;
}

int luaFailure(lua_State *lua, const restricted::Budget &budget, int status, char **error) {
  if (status == LUA_ERRMEM)
    return fail(error, QLD_OUT_OF_MEMORY, budget.allocationLimitHit ?
        "backend manifest Lua allocation limit exceeded" : "backend manifest out of memory");
  if (budget.instructions >= restricted::maxInstructions)
    return fail(error, QLD_ERROR, "backend manifest instruction limit exceeded");
  size_t length = 0;
  if (lua_type(lua, -1) == LUA_TSTRING) {
    const char *message = lua_tolstring(lua, -1, &length);
    if (length && message[0]) return fail(error, QLD_PARSE_ERROR, {message, length});
  }
  return fail(error, QLD_PARSE_ERROR, "backend manifest Lua error");
}
} // namespace

int qld_backend_manifest(const char *source, size_t source_size,
                         uint8_t **out_bytes, size_t *out_size, char **errmsg) {
  if (out_bytes) *out_bytes = nullptr;
  if (out_size) *out_size = 0;
  if (errmsg) *errmsg = nullptr;
  if (!source || !out_bytes || !out_size)
    return fail(errmsg, QLD_INVALID_ARGUMENT, "null backend manifest source or output");
  if (source_size > restricted::maxSource)
    return fail(errmsg, QLD_ERROR, "backend manifest source size limit exceeded");
  if (std::memchr(source, '\0', source_size))
    return fail(errmsg, QLD_PARSE_ERROR, "NUL in backend manifest source");

  Manifest manifest;
  restricted::Budget budget{.name = "backend manifest"};
  auto lua = restricted::create(budget, luaL_makeseed(nullptr));
  if (!lua) return fail(errmsg, QLD_OUT_OF_MEMORY, "cannot allocate backend manifest Lua state");
  // All operations that can raise a Lua error run under lua_pcall or the
  // loader's own protected parser; the state stays owned in this caller.
  lua_pushcfunction(lua.get(), initialize);
  lua_pushlightuserdata(lua.get(), &manifest);
  int status = lua_pcall(lua.get(), 1, 0, 0);
  if (status == LUA_OK)
    status = luaL_loadbufferx(lua.get(), source, source_size, "=backend manifest", "t");
  if (status == LUA_OK) {
    lua_sethook(lua.get(), restricted::countInstructions, LUA_MASKCOUNT, restricted::hookInterval);
    status = lua_pcall(lua.get(), 0, 0, 0);
  }
  if (status != LUA_OK) return luaFailure(lua.get(), budget, status, errmsg);
  if (!manifest.count)
    return fail(errmsg, QLD_PARSE_ERROR, "backend manifest registered no backends");

  auto *bytes = static_cast<uint8_t *>(std::malloc(manifest.size));
  if (!bytes) return fail(errmsg, QLD_OUT_OF_MEMORY, "backend manifest out of memory");
  std::memcpy(bytes, manifest.bytes.data(), manifest.size);
  *out_bytes = bytes;
  *out_size = manifest.size;
  return QLD_OK;
}
