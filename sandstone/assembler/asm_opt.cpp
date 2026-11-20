/* Isolated Lua runtime for the optional post-expansion assembly hook. */
#include "intern.h"
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

struct sand_asm_opt_hook {
    std::string script;
    std::string chunk_name;
};

namespace {
constexpr size_t text_limit = 16 * 1024 * 1024;
constexpr size_t lua_memory_limit = 128 * 1024 * 1024;
constexpr int tick_interval = 1000;
constexpr unsigned instruction_limit = 50'000'000;

struct Budget {
    size_t bytes = 0;
    unsigned ticks = instruction_limit / tick_interval;
};

void *allocate(void *user, void *ptr, size_t before, size_t after) {
    auto &budget = *static_cast<Budget *>(user);
    // On a new allocation Lua passes an object type, not a size, as `before`.
    size_t retained = budget.bytes - (ptr ? before : 0);
    if (!after) {
        std::free(ptr);
        budget.bytes = retained;
        return nullptr;
    }
    if (after > lua_memory_limit - retained)
        return nullptr;
    void *result = std::realloc(ptr, after);
    if (result)
        budget.bytes = retained + after;
    return result;
}

void tick(lua_State *lua, lua_Debug *) {
    void *user;
    lua_getallocf(lua, &user);
    auto &budget = *static_cast<Budget *>(user);
    if (budget.ticks && --budget.ticks)
        return;
    // An inner pcall must not let the script continue after exhaustion.
    lua_sethook(lua, tick, LUA_MASKCOUNT, 1);
    luaL_error(lua, "Lua instruction limit exceeded");
}

struct VM {
    Budget budget;
    std::unique_ptr<lua_State, decltype(&lua_close)> state;

    VM() :
#if LUA_VERSION_NUM >= 505
        state(lua_newstate(allocate, &budget, luaL_makeseed(nullptr)), lua_close)
#else
        state(lua_newstate(allocate, &budget), lua_close)
#endif
    {
        if (!state)
            throw std::runtime_error("cannot create Lua state");
    }

    lua_State *get() const { return state.get(); }

    void check(int status) const {
        if (status == LUA_OK)
            return;
        if (status == LUA_ERRMEM)
            throw std::runtime_error("Lua memory allocation failed (128 MiB limit)");
        // Avoid converting arbitrary error objects or allocating in Lua here.
        const char *detail = lua_type(get(), -1) == LUA_TSTRING ? lua_tostring(get(), -1) : nullptr;
        throw std::runtime_error(detail && *detail ? detail : "Lua failed without an error message");
    }

    void compile(const sand_asm_opt_hook &hook) const {
        check(luaL_loadbufferx(get(), hook.script.data(), hook.script.size(), hook.chunk_name.c_str(), "t"));
    }
};

struct Arguments {
    const char *source;
    const char *name;
    const char *arch;
    const char *syntax;
    bool riscv64_nop_alias;
};

const char *bank_arch(const DomSymAs_Bank *bank) {
    const auto *first = domsymas_target_at(bank, 0);
    if (!first || !first->arch || !*first->arch)
        return nullptr;
    for (uint32_t i = 1, n = domsymas_target_count(bank); i < n; ++i) {
        const auto *target = domsymas_target_at(bank, i);
        if (!target || !target->arch || std::strcmp(first->arch, target->arch))
            return nullptr;
    }
    return first->arch;
}

bool rv64_nop_alias(const sandstone_asm_t *ctx, const char *arch) {
    if (!arch || std::strcmp(arch, "riscv64"))
        return false;
    uint8_t addi[4], spaced[4], nop[4];
    return sand_encode_literal(ctx, "addi x0,x0,0", addi, sizeof addi) &&
           sand_encode_literal(ctx, "addi x0, x0, 0", spaced, sizeof spaced) &&
           sand_encode_literal(ctx, "nop", nop, sizeof nop) &&
           std::memcmp(addi, nop, sizeof nop) == 0 &&
           std::memcmp(spaced, nop, sizeof nop) == 0;
}

int readonly_context(lua_State *lua) {
    return luaL_error(lua, "asm_context is read-only");
}

void expose_context(lua_State *lua, const Arguments &args) {
    lua_newtable(lua); // Empty proxy; all field writes go through __newindex.
    lua_createtable(lua, 0, 3);
    lua_createtable(lua, 0, 3);
    lua_pushstring(lua, args.arch);
    lua_setfield(lua, -2, "arch");
    lua_pushstring(lua, args.syntax);
    lua_setfield(lua, -2, "syntax");
    lua_pushboolean(lua, args.riscv64_nop_alias);
    lua_setfield(lua, -2, "riscv64_nop_alias");
    lua_setfield(lua, -2, "__index");
    lua_pushcfunction(lua, readonly_context);
    lua_setfield(lua, -2, "__newindex");
    lua_pushboolean(lua, 0);
    lua_setfield(lua, -2, "__metatable");
    lua_setmetatable(lua, -2);
    lua_setglobal(lua, "asm_context");
}

// Lua errors can longjmp. This callback runs inside lua_pcall and has no
// C++ objects requiring destruction.
int invoke(lua_State *lua) {
    auto *args = static_cast<const Arguments *>(lua_touserdata(lua, 2));
    const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (const auto &lib : libs) {
        luaL_requiref(lua, lib.name, lib.func, 1);
        lua_pop(lua, 1);
    }
    // No file/console access or dynamic chunk loading. Disallow GC
    // finalizers and error handlers, which can run with debug hooks disabled,
    // and raw writes that would bypass the read-only context.
    for (const char *name : {"dofile", "loadfile", "load", "print", "warn",
                             "setmetatable", "xpcall", "rawset"}) {
        lua_pushnil(lua);
        lua_setglobal(lua, name);
    }
    expose_context(lua, *args);

    lua_pushvalue(lua, 1);
    lua_call(lua, 0, 0);
    lua_getglobal(lua, "asm_optimize");
    if (!lua_isfunction(lua, -1))
        return luaL_error(lua, "hook must define asm_optimize(source, name)");
    lua_pushstring(lua, args->source);
    lua_pushstring(lua, args->name);
    lua_call(lua, 2, 1);
    return 1;
}

void install(sandstone_asm_t *ctx, std::string script, std::string chunk_name) {
    if (script.empty())
        throw std::runtime_error("Lua script is empty");
    if (script.find('\0') != std::string::npos)
        throw std::runtime_error("NUL in Lua script");
    auto next = std::make_unique<sand_asm_opt_hook>(sand_asm_opt_hook{std::move(script), std::move(chunk_name)});
    VM vm;
    vm.compile(*next); // Validate syntax without executing the chunk.
    sandstone_asm_clear_asm_opt_hook(ctx);
    ctx->asm_opt_hook = next.release();
}
}

int sandstone_asm_set_asm_opt_hook(sandstone_asm_t *ctx, const char *source, size_t size) {
    if (!ctx) return -1;
    try {
        if (!source || !size)
            throw std::runtime_error("no Lua script supplied");
        if (size > text_limit)
            throw std::runtime_error("Lua script exceeds 16 MiB");
        install(ctx, std::string(source, size), "=asm-opt");
        return 0;
    } catch (const std::exception &e) {
        sand_set_error(ctx, "asm-opt: %s", e.what());
        return -1;
    }
}

int sandstone_asm_load_asm_opt_hook(sandstone_asm_t *ctx, const char *path) {
    if (!ctx) return -1;
    if (!path || !*path) {
        sand_set_error(ctx, "asm-opt: no Lua script path supplied");
        return -1;
    }
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("cannot open Lua script");
        const auto size = file.tellg();
        if (size < 0)
            throw std::runtime_error("cannot read Lua script");
        if (size > static_cast<std::streamoff>(text_limit))
            throw std::runtime_error("Lua script exceeds 16 MiB");
        std::string script(static_cast<size_t>(size), '\0');
        file.seekg(0);
        if (!file.read(script.data(), static_cast<std::streamsize>(script.size())))
            throw std::runtime_error("cannot read Lua script");
        install(ctx, std::move(script), std::string("@") + path);
        return 0;
    } catch (const std::exception &e) {
        sand_set_error(ctx, "asm-opt '%s': %s", path, e.what());
        return -1;
    }
}

void sandstone_asm_clear_asm_opt_hook(sandstone_asm_t *ctx) {
    if (!ctx) return;
    delete ctx->asm_opt_hook;
    ctx->asm_opt_hook = nullptr;
}

char *sand_run_asm_opt_hook(sandstone_asm_t *ctx, const char *source, const char *name) {
    try {
        if (!ctx->asm_opt_hook)
            throw std::runtime_error("no hook configured");
        VM vm;
        vm.compile(*ctx->asm_opt_hook);
        lua_State *lua = vm.get();
        const char *arch = bank_arch(ctx->bank);
        const char *syntax = ctx->syntax_name && *ctx->syntax_name ? ctx->syntax_name : nullptr;
        Arguments args{source, name, arch, syntax, rv64_nop_alias(ctx, arch)};
        lua_pushcfunction(lua, invoke);
        lua_insert(lua, -2);
        lua_pushlightuserdata(lua, &args);
        lua_sethook(lua, tick, LUA_MASKCOUNT, tick_interval);
        const int status = lua_pcall(lua, 2, 1, 0);
        if (!vm.budget.ticks)
            throw std::runtime_error("Lua instruction limit exceeded");
        vm.check(status);

        if (lua_type(lua, -1) != LUA_TSTRING)
            throw std::runtime_error("asm_optimize must return a string");
        size_t size;
        const char *text = lua_tolstring(lua, -1, &size);
        if (size > text_limit)
            throw std::runtime_error("asm_optimize output exceeds 16 MiB");
        if (std::memchr(text, '\0', size))
            throw std::runtime_error("NUL in asm_optimize output");
        char *result = sand_strndup(text, size);
        if (!result)
            throw std::bad_alloc();
        return result;
    } catch (const std::exception &e) {
        sand_set_error(ctx, "%s: asm-opt: %s", name, e.what());
        return nullptr;
    }
}
