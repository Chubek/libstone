#include "lua_extensions.hpp"

#include "cephyr/CephyrSDK.h"
#include "cephyr/diagnostics.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <kaguya/config.hpp> // utility.hpp tests configuration before including it
#include <kaguya/utility.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace cephyr {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t memory_limit = 32u * 1024u * 1024u;
constexpr auto time_limit = std::chrono::seconds(2);
constexpr int instruction_interval = 1000;
constexpr int instruction_limit = 1'000'000;
constexpr std::size_t max_find_needle = 256;

std::string_view view(cephyr_sdk_text text) noexcept {
    return text.size ? std::string_view(text.data, text.size) : std::string_view{};
}

cephyr_sdk_text span(std::string_view text) noexcept {
    return {text.data(), text.size()};
}

bool ascii_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool identifier(std::string_view name) noexcept {
    return !name.empty() && (ascii_alpha(name.front()) || name.front() == '_') &&
           std::ranges::all_of(name, [](char c) {
               return ascii_alpha(c) || ascii_digit(c) || c == '_';
           });
}

bool hook_name(std::string_view name) noexcept {
    return !name.empty() && std::ranges::all_of(name, [](char c) {
        return ascii_alpha(c) || ascii_digit(c) || c == '_' || c == '.' || c == '-';
    });
}

const char *stage_name(std::uint32_t stage) noexcept {
    switch (stage) {
    case CEPHYR_SDK_DESUGAR: return "desugar";
    case CEPHYR_SDK_ANALYZE: return "analyze";
    case CEPHYR_SDK_REWRITE: return "rewrite";
    case CEPHYR_SDK_DIRECTIVE: return "directive";
    default: return "unknown";
    }
}

struct Script;

struct Registration {
    Script *script = nullptr;
    std::uint32_t stage = 0;
    int function = LUA_NOREF;
    std::array<char, CEPHYR_SDK_MAX_NAME_BYTES> name{};
    std::array<char, CEPHYR_SDK_MAX_TRIPLE_BYTES> triple{};
    std::size_t name_size = 0;
    std::size_t triple_size = 0;
};

struct Invocation {
    const cephyr_sdk_input *input;
    const cephyr_sdk_output *output;
    int function;
    bool emitted = false;
};

struct Script {
    struct EnvironmentEntry {
        std::string name;
        std::string value;
    };

    std::string name;
    std::vector<EnvironmentEntry> environment;
    std::array<Registration, CEPHYR_SDK_MAX_HOOKS + CEPHYR_SDK_MAX_DIRECTIVES> registrations{};
    std::size_t hook_count = 0;
    std::size_t directive_count = 0;
    std::size_t hook_limit = 0;
    std::size_t directive_limit = 0;
    lua_State *lua = nullptr;
    std::size_t bytes = 0;
    int instructions_left = instruction_limit;
    Clock::duration time_left = time_limit;
    Clock::time_point deadline{};
    const char *quota_error = nullptr;
    Invocation *active = nullptr;
    bool registering = true;

    ~Script() {
        if (lua)
            lua_close(lua);
    }

    std::string_view run(lua_CFunction trampoline, void *request) noexcept;
};

Script &script(lua_State *L) noexcept {
    void *user = nullptr;
    lua_getallocf(L, &user);
    return *static_cast<Script *>(user);
}

void *allocate(void *user, void *ptr, std::size_t old_size, std::size_t new_size) noexcept {
    auto &state = *static_cast<Script *>(user);
    // For a new allocation Lua may use old_size to communicate the object type.
    const auto retained = state.bytes - (ptr ? old_size : 0);
    if (!new_size) {
        std::free(ptr);
        state.bytes = retained;
        return nullptr;
    }
    if (new_size > memory_limit - retained)
        return nullptr;
    void *result = std::realloc(ptr, new_size);
    if (result)
        state.bytes = retained + new_size;
    return result;
}

void instruction_hook(lua_State *L, lua_Debug *) {
    auto &state = script(L);
    if ((state.instructions_left -= instruction_interval) <= 0)
        state.quota_error = "Lua instruction limit exceeded";
    else if (Clock::now() >= state.deadline)
        state.quota_error = "Lua execution time limit exceeded";
    if (state.quota_error)
        luaL_error(L, "%s", state.quota_error);
}

std::string_view Script::run(lua_CFunction trampoline, void *request) noexcept {
    if (quota_error)
        return quota_error;
    // Charge for the host work and for invocations shorter than the hook interval.
    if ((instructions_left -= instruction_interval) <= 0) {
        quota_error = "Lua instruction limit exceeded";
        return quota_error;
    }
    const auto started = Clock::now();
    deadline = started + time_left;
    // These two pushes allocate nothing and the base frame has room. All Lua
    // allocations and user execution below them occur inside lua_pcall.
    lua_pushcfunction(lua, trampoline);
    lua_pushlightuserdata(lua, request);
    const int status = lua_pcall(lua, 1, 0, 0);
    const auto elapsed = Clock::now() - started;
    if (!quota_error && elapsed >= time_left)
        quota_error = "Lua execution time limit exceeded";
    if (quota_error)
        return quota_error;
    time_left -= elapsed;
    if (status == LUA_OK)
        return {};
    if (status == LUA_ERRMEM) {
        quota_error = "Lua memory limit exceeded or allocation failed";
        return quota_error;
    }
    if (lua_type(lua, -1) == LUA_TSTRING) {
        std::size_t size = 0;
        const char *message = lua_tolstring(lua, -1, &size);
        if (size)
            return {message, std::min<std::size_t>(size, CEPHYR_SDK_MAX_MESSAGE_BYTES)};
    }
    return "Lua raised an error without a string message";
}

// Lua can use longjmp here. Keep only trivially destructible values between
// Lua calls and their protected boundary. Owning state is prepared outside it.
std::string_view check_string(lua_State *L, int index) {
    luaL_checktype(L, index, LUA_TSTRING);
    std::size_t size = 0;
    const char *data = lua_tolstring(L, index, &size);
    return {data, size};
}

std::string_view optional_string(lua_State *L, int index) {
    return lua_isnoneornil(L, index) ? std::string_view{} : check_string(L, index);
}

void push_string(lua_State *L, std::string_view text) {
    lua_pushlstring(L, text.empty() ? "" : text.data(), text.size());
}

void text_field(lua_State *L, const char *key, cephyr_sdk_text text) {
    push_string(L, view(text));
    lua_setfield(L, -2, key);
}

void integer_field(lua_State *L, const char *key, std::uint32_t number) {
    lua_pushinteger(L, static_cast<lua_Integer>(number));
    lua_setfield(L, -2, key);
}

int readonly_assignment(lua_State *L) {
    return luaL_error(L, "lcephyr values are read-only");
}

// Replace the table on top of the stack with a proxy whose backing table is
// never given to the script. The sandbox exposes neither rawset nor metatables.
void make_readonly(lua_State *L) {
    lua_newtable(L); // proxy
    lua_createtable(L, 0, 3); // metatable
    lua_pushvalue(L, -3); // backing table
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, readonly_assignment);
    lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_remove(L, -2);
}

char module_key;

int require_module(lua_State *L) {
    if (check_string(L, 1) != "lcephyr")
        return luaL_error(L, "only require('lcephyr') is available");
    lua_rawgetp(L, LUA_REGISTRYINDEX, &module_key);
    return 1;
}

int env_get(lua_State *L) {
    const auto key = check_string(L, 1);
    const auto &environment = script(L).environment;
    if (key.size() <= CEPHYR_LUA_MAX_ENV_NAME_BYTES) {
        for (std::size_t i = 0; i < environment.size(); ++i) {
            if (environment[i].name == key) {
                push_string(L, environment[i].value);
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

int env_list(lua_State *L) {
    const auto &environment = script(L).environment;
    lua_createtable(L, static_cast<int>(environment.size()), 0);
    for (std::size_t i = 0; i < environment.size(); ++i) {
        push_string(L, environment[i].name);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int register_callback(lua_State *L, std::uint32_t stage, int name_index) {
    auto &state = script(L);
    if (!state.registering)
        return luaL_error(L, "callbacks can only be registered while loading the script");
    const bool directive = stage == CEPHYR_SDK_DIRECTIVE;
    const auto name = check_string(L, name_index);
    luaL_argcheck(L, name.size() <= CEPHYR_SDK_MAX_NAME_BYTES &&
                    (directive ? identifier(name) : hook_name(name)),
                  name_index, "invalid callback name");
    const int function_index = name_index + 1;
    luaL_checktype(L, function_index, LUA_TFUNCTION);
    const auto triple = optional_string(L, name_index + 2);
    luaL_argcheck(L, triple.size() <= CEPHYR_SDK_MAX_TRIPLE_BYTES &&
                    triple.find('\0') == std::string_view::npos,
                  name_index + 2, "invalid target triple");
    if (directive ? state.directive_count >= state.directive_limit :
                    state.hook_count >= state.hook_limit)
        return luaL_error(L, "SDK registration limit exceeded");
    lua_pushvalue(L, function_index);
    const int function = luaL_ref(L, LUA_REGISTRYINDEX);
    auto &entry = state.registrations[state.hook_count + state.directive_count];
    entry.script = &state;
    entry.stage = stage;
    entry.function = function;
    entry.name_size = name.size();
    entry.triple_size = triple.size();
    std::memcpy(entry.name.data(), name.data(), name.size());
    if (!triple.empty())
        std::memcpy(entry.triple.data(), triple.data(), triple.size());
    if (directive)
        ++state.directive_count;
    else
        ++state.hook_count;
    return 0;
}

int register_hook(lua_State *L) {
    const auto stage = check_string(L, 1);
    if (stage == "desugar")
        return register_callback(L, CEPHYR_SDK_DESUGAR, 2);
    if (stage == "analyze")
        return register_callback(L, CEPHYR_SDK_ANALYZE, 2);
    if (stage == "rewrite")
        return register_callback(L, CEPHYR_SDK_REWRITE, 2);
    return luaL_argerror(L, 1, "expected 'desugar', 'analyze', or 'rewrite'");
}

int register_directive(lua_State *L) {
    return register_callback(L, CEPHYR_SDK_DIRECTIVE, 1);
}

std::uint32_t coordinate(lua_State *L, int index) {
    if (lua_isnoneornil(L, index))
        return 0;
    luaL_argcheck(L, lua_isinteger(L, index), index, "expected an integer coordinate");
    const auto value = lua_tointeger(L, index);
    luaL_argcheck(L, value >= 0 && value <= std::numeric_limits<std::uint32_t>::max(),
                  index, "coordinate out of range");
    return static_cast<std::uint32_t>(value);
}

int report(lua_State *L) {
    const auto *active = script(L).active;
    if (!active)
        return luaL_error(L, "report is available only during a callback");
    const auto severity = check_string(L, 1);
    const auto level = severity == "note" ? CEPHYR_SDK_NOTE :
                       severity == "warning" ? CEPHYR_SDK_WARNING :
                       severity == "error" ? CEPHYR_SDK_ERROR : 0;
    luaL_argcheck(L, level != 0, 1, "expected 'note', 'warning', or 'error'");
    const auto code = check_string(L, 2);
    const auto message = check_string(L, 3);
    const auto file = optional_string(L, 4);
    const auto line = coordinate(L, 5);
    const auto column = coordinate(L, 6);
    const auto &output = *active->output;
    if (!output.report(output.context, static_cast<std::uint32_t>(level),
                       span(code), span(message), span(file), line, column))
        return luaL_error(L, "SDK diagnostic was rejected");
    return 0;
}

int define_integer(lua_State *L) {
    auto *active = script(L).active;
    if (!active || active->input->stage != CEPHYR_SDK_DIRECTIVE)
        return luaL_error(L, "define_integer is available only during a directive callback");
    if (active->emitted)
        return luaL_error(L, "define_integer may be called only once per directive");

    const auto name = check_string(L, 1);
    luaL_argcheck(L, name.size() <= CEPHYR_SDK_MAX_NAME_BYTES && identifier(name),
                  1, "expected an ASCII C identifier");
    luaL_argcheck(L, lua_isinteger(L, 2), 2, "expected a Lua integer");
    const auto value = lua_tointeger(L, 2);
    luaL_argcheck(L, value >= 0 && value <= 2147483647, 2,
                  "expected an integer between 0 and 2147483647");

    // Emit a complete declaration from validated tokens, never arbitrary text.
    constexpr std::string_view prefix = "enum { ";
    constexpr std::string_view equals = " = ";
    constexpr std::string_view suffix = " };";
    char text[prefix.size() + CEPHYR_SDK_MAX_NAME_BYTES + equals.size() + 10 + suffix.size()];
    char *end = std::copy(prefix.begin(), prefix.end(), text);
    end = std::copy(name.begin(), name.end(), end);
    end = std::copy(equals.begin(), equals.end(), end);
    end = std::to_chars(end, text + sizeof(text) - suffix.size(),
                        static_cast<std::uint32_t>(value)).ptr;
    end = std::copy(suffix.begin(), suffix.end(), end);
    const auto &output = *active->output;
    if (!output.set_text(output.context, {text, static_cast<std::size_t>(end - text)}))
        return luaL_error(L, "SDK directive output was rejected");
    active->emitted = true;
    return 0;
}

int safe_tostring(lua_State *L) {
    switch (lua_type(L, 1)) {
    case LUA_TSTRING:
    case LUA_TNUMBER:
        lua_tolstring(L, 1, nullptr);
        lua_pushvalue(L, 1);
        break;
    case LUA_TBOOLEAN:
        lua_pushstring(L, lua_toboolean(L, 1) ? "true" : "false");
        break;
    default:
        luaL_checkany(L, 1);
        lua_pushstring(L, luaL_typename(L, 1));
        break;
    }
    return 1;
}

int literal_find(lua_State *L) {
    const auto subject = check_string(L, 1);
    const auto needle = check_string(L, 2);
    const auto length = static_cast<lua_Integer>(subject.size());
    auto start = luaL_optinteger(L, 3, 1);
    luaL_argcheck(L, lua_isnoneornil(L, 4) ||
                    (lua_isboolean(L, 4) && lua_toboolean(L, 4)),
                  4, "Lua patterns are not available");
    luaL_argcheck(L, needle.size() <= max_find_needle, 2, "literal search needle exceeds 256 bytes");
    if (start < 0)
        start = start < -length ? 1 : length + start + 1;
    if (start < 1)
        start = 1;
    if (start - 1 <= length) {
        if (needle.empty()) {
            lua_pushinteger(L, start);
            lua_pushinteger(L, start - 1);
            return 2;
        }
        // A linear literal search avoids the unmetered Lua pattern engine.
        std::array<std::size_t, max_find_needle> prefix{};
        for (std::size_t i = 1, matched = 0; i < needle.size(); ++i) {
            while (matched && needle[i] != needle[matched])
                matched = prefix[matched - 1];
            if (needle[i] == needle[matched])
                ++matched;
            prefix[i] = matched;
        }
        for (std::size_t i = static_cast<std::size_t>(start - 1), matched = 0;
             i < subject.size(); ++i) {
            while (matched && subject[i] != needle[matched])
                matched = prefix[matched - 1];
            if (subject[i] == needle[matched] && ++matched == needle.size()) {
                lua_pushinteger(L, static_cast<lua_Integer>(i + 2 - needle.size()));
                lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
                return 2;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

int bounded_rep(lua_State *L) {
    const auto text = check_string(L, 1);
    const auto count = luaL_checkinteger(L, 2);
    const auto separator = optional_string(L, 3);
    // The standard implementation may loop count times without allocating
    // when both strings are empty, so handle this case without calling it.
    if (count <= 0 || (text.empty() && separator.empty())) {
        lua_pushliteral(L, "");
        return 1;
    }
    lua_settop(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, 3, 1);
    return 1;
}

void copy_fields(lua_State *L, int from, int to, std::span<const char *const> fields) {
    for (const char *field : fields) {
        lua_getfield(L, from, field);
        lua_setfield(L, to, field);
    }
}

void open_selected_library(lua_State *L, int globals, const char *name,
                           lua_CFunction open, std::span<const char *const> fields) {
    open(L);
    const int original = lua_gettop(L);
    lua_createtable(L, 0, static_cast<int>(fields.size()));
    copy_fields(L, original, lua_gettop(L), fields);
    lua_setfield(L, globals, name);
    lua_pop(L, 1); // original library
}

void open_sandbox(lua_State *L) {
    // Use positive absolute stack indices while selecting from vendor libraries.
    // A new function frame starts with at least LUA_MINSTACK free slots.
    const int base = lua_gettop(L);
    luaopen_base(L);
    const int original_globals = lua_gettop(L);
    lua_createtable(L, 0, 20);
    const int globals = lua_gettop(L);
    static constexpr const char *base_fields[]{
        "_VERSION", "assert", "error", "ipairs", "next", "pairs", "rawequal",
        "rawget", "rawlen", "select", "tonumber", "type"
    };
    copy_fields(L, original_globals, globals, base_fields);
    lua_pushvalue(L, globals);
    lua_setfield(L, globals, "_G");
    lua_pushcfunction(L, safe_tostring);
    lua_setfield(L, globals, "tostring");
    lua_pushcfunction(L, require_module);
    lua_setfield(L, globals, "require");

    luaopen_string(L);
    const int original_string = lua_gettop(L);
    lua_createtable(L, 0, 9);
    const int sandbox_string = lua_gettop(L);
    static constexpr const char *string_fields[]{
        "byte", "char", "len", "lower", "reverse", "sub", "upper"
    };
    copy_fields(L, original_string, sandbox_string, string_fields);
    lua_pushcfunction(L, literal_find);
    lua_setfield(L, sandbox_string, "find");
    lua_getfield(L, original_string, "rep");
    lua_pushcclosure(L, bounded_rep, 1);
    lua_setfield(L, sandbox_string, "rep");
    // String method lookup must use the same restricted table. Opening the
    // vendor library just installed this string metatable on the state.
    lua_pushliteral(L, "");
    lua_getmetatable(L, -1);
    lua_pushvalue(L, sandbox_string);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 2); // string metatable and dummy string
    lua_setfield(L, globals, "string");
    lua_pop(L, 1); // original string library

    static constexpr const char *table_fields[]{"concat", "insert", "pack", "remove", "unpack"};
    static constexpr const char *math_fields[]{
        "abs", "ceil", "floor", "huge", "max", "maxinteger", "min", "mininteger",
        "pi", "tointeger", "type"
    };
    open_selected_library(L, globals, "table", luaopen_table, table_fields);
    open_selected_library(L, globals, "math", luaopen_math, math_fields);

    lua_pushvalue(L, globals);
    lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_settop(L, base);

    static constexpr luaL_Reg module[] = {
        {"on", register_hook}, {"directive", register_directive}, {"report", report},
        {"define_integer", define_integer}, {nullptr, nullptr}
    };
    static constexpr luaL_Reg env[] = {
        {"get", env_get}, {"list", env_list}, {nullptr, nullptr}
    };
    luaL_newlib(L, module);
    luaL_newlib(L, env);
    make_readonly(L);
    lua_setfield(L, -2, "env");
    make_readonly(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &module_key);
}

struct LoadRequest {
    std::string_view source;
    const char *chunk_name;
};

int load_script(lua_State *L) {
    const auto &request = *static_cast<const LoadRequest *>(lua_touserdata(L, 1));
    open_sandbox(L);
    const auto source = request.source;
    if (luaL_loadbufferx(L, source.empty() ? "" : source.data(), source.size(),
                         request.chunk_name, "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 0);
    return 0;
}

int call_script(lua_State *L) {
    const auto &call = *static_cast<const Invocation *>(lua_touserdata(L, 1));
    const auto &input = *call.input;
    lua_rawgeti(L, LUA_REGISTRYINDEX, call.function);
    lua_createtable(L, 0, 11);
    lua_pushstring(L, stage_name(input.stage));
    lua_setfield(L, -2, "stage");
    text_field(L, "text", input.text);
    text_field(L, "input_name", input.input_name);
    text_field(L, "target_triple", input.target_triple);
    text_field(L, "target_architecture", input.target_architecture);
    text_field(L, "target_abi", input.target_abi);
    integer_field(L, "mir_schema_version", input.mir_schema_version);
    integer_field(L, "flags", input.flags);
    if (input.stage == CEPHYR_SDK_DIRECTIVE) {
        text_field(L, "directive_name", input.directive_name);
        integer_field(L, "line", input.line);
        integer_field(L, "column", input.column);
    }
    make_readonly(L);
    lua_call(L, 1, 0);
    return 0;
}

std::uint32_t invoke(const cephyr_sdk_input *input, const cephyr_sdk_output *output,
                     void *user) noexcept {
    const auto &entry = *static_cast<const Registration *>(user);
    auto &state = *entry.script;
    // Only lua_pcall's result or error can be left in this outer frame. There
    // are no to-be-closed slots here, so the guard's lua_settop cannot raise.
    const kaguya::util::ScopedSavedStack restore_stack(state.lua);
    Invocation call{input, output, entry.function};
    state.active = &call;
    const auto failure = state.run(call_script, &call);
    state.active = nullptr;
    if (failure.empty())
        return CEPHYR_SDK_OK;
    char message[CEPHYR_SDK_MAX_MESSAGE_BYTES];
    const auto size = std::min(failure.size(), sizeof(message));
    std::replace_copy(failure.begin(), failure.begin() + size, message, '\0', '?');
    output->report(output->context, CEPHYR_SDK_ERROR, span("cephyr.lua.callback"),
                   {message, size}, span(state.name), 0, 0);
    return CEPHYR_SDK_REJECT;
}

} // namespace

struct LuaExtensions::Impl {
    std::array<cephyr_sdk_hook, CEPHYR_SDK_MAX_HOOKS> hooks{};
    std::array<cephyr_sdk_directive, CEPHYR_SDK_MAX_DIRECTIVES> directives{};
    std::array<std::unique_ptr<Script>, CEPHYR_LUA_MAX_SCRIPTS> scripts;
    std::size_t script_count = 0;
    cephyr_sdk sdk{sizeof(cephyr_sdk), CEPHYR_SDK_ABI_VERSION, hooks.data(), 0,
                   directives.data(), 0};
};

LuaExtensions::LuaExtensions() = default;
LuaExtensions::~LuaExtensions() = default;

bool LuaExtensions::set_native(const cephyr_sdk *native, Diagnostics &diagnostics) {
    if (impl_ && impl_->script_count) {
        diagnostics.error("cephyr.lua.registration", "set the native SDK before loading Lua scripts");
        return false;
    }
    if (!native) {
        impl_.reset();
        return true;
    }
    const auto invalid = [&diagnostics] {
        diagnostics.error("cephyr.sdk.registration", "invalid native SDK descriptor or registration arrays");
        return false;
    };
    if (native->struct_size < offsetof(cephyr_sdk, directives) ||
        native->abi_version != CEPHYR_SDK_ABI_VERSION ||
        native->hook_count > CEPHYR_SDK_MAX_HOOKS ||
        (native->hook_count && !native->hooks))
        return invalid();

    constexpr auto directive_fields_size = offsetof(cephyr_sdk, directive_count) +
                                           sizeof(cephyr_sdk::directive_count);
    const bool has_directives = native->struct_size >= directive_fields_size;
    const auto directive_count = has_directives ? native->directive_count : 0;
    if (directive_count > CEPHYR_SDK_MAX_DIRECTIVES ||
        (directive_count && !native->directives))
        return invalid();

    if (!impl_)
        impl_ = std::make_unique<Impl>();
    if (native->hook_count)
        std::copy_n(native->hooks, native->hook_count, impl_->hooks.begin());
    if (directive_count)
        std::copy_n(native->directives, directive_count, impl_->directives.begin());
    impl_->sdk.hook_count = native->hook_count;
    impl_->sdk.directive_count = directive_count;
    return true;
}

bool LuaExtensions::add(std::string_view source, std::string_view name,
                        std::span<const cephyr_lua_env_entry> environment,
                        Diagnostics &diagnostics) {
    if (source.size() > CEPHYR_LUA_MAX_SCRIPT_BYTES ||
        name.size() > CEPHYR_LUA_MAX_NAME_BYTES || name.find('\0') != name.npos ||
        environment.size() > CEPHYR_LUA_MAX_ENV_ENTRIES) {
        diagnostics.error("cephyr.lua.load", "Lua script, name or environment exceeds its limits");
        return false;
    }
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    if (impl_->script_count == CEPHYR_LUA_MAX_SCRIPTS) {
        diagnostics.error("cephyr.lua.load", "Lua script count limit exceeded");
        return false;
    }
    auto state = std::make_unique<Script>();
    state->name = name.empty() ? "<lua extension>" : name;
    const auto error = [&](std::string_view message) {
        diagnostics.error("cephyr.lua.load", message, Location{state->name});
        return false;
    };
    state->environment.reserve(environment.size());
    for (const auto &entry : environment) {
        if (!entry.name || !entry.value)
            return error("invalid Lua environment entry");
        std::string_view key(entry.name);
        std::string_view value(entry.value);
        if (key.size() > CEPHYR_LUA_MAX_ENV_NAME_BYTES || !identifier(key) ||
            value.size() > CEPHYR_LUA_MAX_ENV_VALUE_BYTES)
            return error("invalid Lua environment entry");
        state->environment.push_back({std::string(key), std::string(value)});
    }
    std::ranges::sort(state->environment, {}, &Script::EnvironmentEntry::name);
    for (std::size_t i = 1; i < state->environment.size(); ++i) {
        if (state->environment[i - 1].name == state->environment[i].name)
            return error("duplicate Lua environment name");
    }

#if LUA_VERSION_NUM >= 505
    state->lua = lua_newstate(allocate, state.get(), luaL_makeseed(nullptr));
#else
    state->lua = lua_newstate(allocate, state.get());
#endif
    if (!state->lua)
        return error("could not initialize the Lua state");
    state->hook_limit = impl_->hooks.size() - impl_->sdk.hook_count;
    state->directive_limit = impl_->directives.size() - impl_->sdk.directive_count;
    lua_sethook(state->lua, instruction_hook, LUA_MASKCOUNT, instruction_interval);
    const std::string chunk_name = "@" + state->name;
    LoadRequest request{source, chunk_name.c_str()};
    const auto failure = state->run(load_script, &request);
    state->registering = false;
    if (!failure.empty())
        return error(failure);

    for (std::size_t i = 0; i < state->hook_count + state->directive_count; ++i) {
        auto &entry = state->registrations[i];
        const cephyr_sdk_text entry_name{entry.name.data(), entry.name_size};
        const cephyr_sdk_text triple{entry.triple.data(), entry.triple_size};
        if (entry.stage == CEPHYR_SDK_DIRECTIVE) {
            impl_->directives[impl_->sdk.directive_count++] = {entry_name, triple, invoke, &entry};
        } else {
            impl_->hooks[impl_->sdk.hook_count++] = {entry.stage, entry_name, triple, invoke, &entry};
        }
    }
    impl_->scripts[impl_->script_count++] = std::move(state);
    return true;
}

const cephyr_sdk *LuaExtensions::sdk() const {
    return impl_ ? &impl_->sdk : nullptr;
}

} // namespace cephyr
