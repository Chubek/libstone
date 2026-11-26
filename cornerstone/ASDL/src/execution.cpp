#include "asdl/execution.hpp"
#include "asdl/schema.hpp"
#include "asdl_execution_source.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <stdexcept>
#include <string_view>

namespace asdl {
namespace {

void set_string(lua_State* lua, const char* key, std::string_view value)
{
    lua_pushlstring(lua, value.data(), value.size());
    lua_setfield(lua, -2, key);
}

void set_optional(lua_State* lua, const char* key, std::string_view value)
{
    if (!value.empty())
        set_string(lua, key, value);
}

void set_flag(lua_State* lua, const char* key, bool flag)
{
    lua_pushboolean(lua, flag);
    lua_setfield(lua, -2, key);
}

void set_names(lua_State* lua, const char* key, const std::vector<std::string>& names)
{
    lua_newtable(lua);
    lua_Integer index = 0;
    for (const auto& name : names) {
        lua_pushlstring(lua, name.data(), name.size());
        lua_rawseti(lua, -2, ++index);
    }
    lua_setfield(lua, -2, key);
}

void set_location(lua_State* lua, const SourceSpan& span, std::string_view fallback)
{
    set_string(lua, "file", span.file.empty() ? fallback : std::string_view(span.file));
    lua_pushinteger(lua, static_cast<lua_Integer>(span.begin.line));
    lua_setfield(lua, -2, "line");
}

void add_refinements(lua_State* lua, lua_Integer& index, std::string_view type,
                     std::string_view tag, const std::vector<Field>& fields,
                     std::string_view fallback)
{
    for (const auto& field : fields) {
        if (!field.executable_refinement)
            continue;
        lua_newtable(lua);
        set_string(lua, "type", type);
        set_optional(lua, "tag", tag);
        set_string(lua, "field", field.name);
        set_string(lua, "name", field.refinement);
        const SourceSpan& span = field.refinement_span.file.empty() ? field.span : field.refinement_span;
        set_location(lua, span, fallback);
        lua_rawseti(lua, -2, ++index);
    }
}

void push_schema(lua_State* lua, const Schema& schema)
{
    lua_newtable(lua);
    lua_newtable(lua);
    lua_Integer index = 0;
    for (const auto& import : schema.imports) {
        if (import.schema)
            continue;
        lua_newtable(lua);
        set_string(lua, "module", import.module);
        set_optional(lua, "alias", import.alias);
        set_location(lua, import.span, schema.file);
        lua_newtable(lua);
        lua_Integer name_index = 0;
        for (const auto& name : import.names) {
            lua_newtable(lua);
            set_string(lua, "name", name.name);
            set_optional(lua, "alias", name.alias);
            lua_rawseti(lua, -2, ++name_index);
        }
        lua_setfield(lua, -2, "names");
        lua_rawseti(lua, -2, ++index);
    }
    lua_setfield(lua, -2, "imports");

    lua_newtable(lua);
    index = 0;
    for (const auto& hook : schema.logistics) {
        lua_newtable(lua);
        set_string(lua, "type", hook.type);
        set_names(lua, "parameters", hook.parameters);
        if (!hook.synthesized.empty()) {
            set_names(lua, "synthesized", hook.synthesized);
            set_flag(lua, "check_inherited", true);
        }
        set_string(lua, "body", hook.body.text);
        set_location(lua, hook.body.span.file.empty() ? hook.span : hook.body.span, schema.file);
        lua_rawseti(lua, -2, ++index);
    }
    lua_setfield(lua, -2, "logistics");

    lua_newtable(lua);
    index = 0;
    for (const auto& def : schema.definitions) {
        add_refinements(lua, index, def.name, {}, def.fields, schema.file);
        add_refinements(lua, index, def.name, {}, def.attributes, schema.file);
        for (const auto& constructor : def.constructors)
            add_refinements(lua, index, def.name, constructor.name, constructor.fields, schema.file);
    }
    lua_setfield(lua, -2, "refinements");
}

[[noreturn]] void throw_error(lua_State* lua)
{
    const char* message = lua_tostring(lua, -1);
    throw std::runtime_error(message ? message : "ASDL execution initialization failed");
}

void call(lua_State* lua, int args, int results)
{
    if (lua_pcall(lua, args, results, 0) != LUA_OK)
        throw_error(lua);
}

} // namespace

void push_execution(lua_State* lua, const Schema& schema)
{
    const int top = lua_gettop(lua);
    try {
        if (luaL_loadbufferx(lua, detail::execution_source, sizeof(detail::execution_source) - 1,
                            "=ASDL execution runtime", "t") != LUA_OK)
            throw_error(lua);
        call(lua, 0, 1);
        lua_getglobal(lua, "asdl");
        if (!lua_istable(lua, -1))
            throw std::runtime_error("ASDL core API must be installed before execution");
        call(lua, 1, 1);
        lua_getfield(lua, -1, "load_schema");
        push_schema(lua, schema);
        call(lua, 1, 0);
    } catch (...) {
        lua_settop(lua, top);
        throw;
    }
}

} // namespace asdl
