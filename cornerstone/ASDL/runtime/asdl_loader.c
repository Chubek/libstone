#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "asdl_loader.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <lua.h>
#include <lauxlib.h>

/* State and metadata live in Lua tables so Lua owns their lifetime.
 * C functions exposed in asdl.lib and asdl.signlib capture this context. */
#define CTX lua_upvalueindex(1)
#define MAX_SOURCE (16u * 1024u * 1024u)

static void set_from(lua_State *L, int dst, const char *key, int src)
{
    dst = lua_absindex(L, dst);
    lua_pushvalue(L, src);
    lua_setfield(L, dst, key);
}

static void copy_table(lua_State *L, int src, int dst)
{
    src = lua_absindex(L, src);
    dst = lua_absindex(L, dst);
    lua_pushnil(L);
    while (lua_next(L, src)) {
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_rawset(L, dst);
        lua_pop(L, 1);
    }
}

static int installed_table(lua_State *L, const char *key)
{
    lua_getfield(L, CTX, key);
    return lua_gettop(L);
}

static const char *check_string(lua_State *L, int idx)
{
    size_t size;
    luaL_checktype(L, idx, LUA_TSTRING);
    const char *value = lua_tolstring(L, idx, &size);
    luaL_argcheck(L, size != 0 && strlen(value) == size, idx,
                  "expected a nonempty string without NUL bytes");
    return value;
}

static int has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

/* Push the real path of a regular file. An absent candidate does not
 * shadow the next root; other filesystem errors fail with a diagnostic. */
static int candidate(lua_State *L, const char *path)
{
    struct stat info;
    char *resolved = realpath(path, NULL);
    if (!resolved) {
        int err = errno;
        if (err == ENOENT || err == ENOTDIR)
            return 0;
        return luaL_error(L, "cannot resolve ASDL library '%s': %s", path, strerror(err));
    }
    if (stat(resolved, &info) != 0 || !S_ISREG(info.st_mode)) {
        free(resolved);
        return luaL_error(L, "ASDL library is not an accessible regular file: %s", path);
    }
    lua_pushstring(L, resolved);
    free(resolved);
    return 1;
}

static void resolve(lua_State *L, const char *name)
{
    int rooted = name[0] == '/' || !strncmp(name, "./", 2) || !strncmp(name, "../", 3);
    int extension = has_suffix(name, ".lua");
    if (rooted) {
        if (candidate(L, extension ? name : lua_pushfstring(L, "%s.lua", name)))
            return;
    } else {
        int literal = extension || strchr(name, '/') != NULL;
        const char *relative = name;
        if (!literal) {
            /* Dotted identifiers have nonempty segments. */
            if (name[0] == '.' || name[strlen(name) - 1] == '.' || strstr(name, ".."))
                luaL_error(L, "invalid dotted ASDL library name '%s'", name);
            luaL_Buffer b;
            luaL_buffinit(L, &b);
            for (const char *p = name; *p; ++p)
                luaL_addchar(&b, *p == '.' ? '/' : *p);
            luaL_addstring(&b, ".lua");
            luaL_pushresult(&b);
            relative = lua_tostring(L, -1);
        } else if (!extension) {
            relative = lua_pushfstring(L, "%s.lua", name);
        }
        int roots = installed_table(L, "roots");
        lua_Integer count = (lua_Integer)lua_rawlen(L, roots);
        for (lua_Integer i = 1; i <= count; ++i) {
            lua_rawgeti(L, roots, i);
            const char *path = lua_pushfstring(L, "%s/%s", lua_tostring(L, -1), relative);
            if (candidate(L, path))
                return;
            lua_pop(L, 2);
        }
    }
    luaL_error(L, "missing ASDL library '%s'", name);
}

static int lib_resolve(lua_State *L)
{
    resolve(L, check_string(L, 1));
    return 1;
}

static int lib_searchpath(lua_State *L)
{
    int roots = installed_table(L, "roots");
    lua_newtable(L);
    copy_table(L, roots, -1);
    return 1;
}

/* Check only the library envelope. Lua itself performs the full parse.
 * The tiny scanner skips comments and recognizes complete string tokens
 * so text within a comment or literal cannot satisfy the envelope. */
enum token_kind { OTHER, WORD, STRING };
struct token { const char *text; size_t length; enum token_kind kind; };

static int ascii_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static size_t long_open(const char *p, const char *end)
{
    if (p == end || *p++ != '[')
        return 0;
    const char *begin = p;
    while (p != end && *p == '=')
        ++p;
    return p != end && *p == '[' ? (size_t)(p - begin) + 2 : 0;
}

static const char *long_end(const char *p, const char *end, size_t size)
{
    p += size;
    while ((size_t)(end - p) >= size) {
        if (*p++ != ']')
            continue;
        size_t i = 0;
        while (i < size - 2 && p[i] == '=')
            ++i;
        if (i == size - 2 && p[i] == ']')
            return p + i + 1;
    }
    return end;
}

static struct token scan(const char **cursor, const char *end)
{
    const char *p = *cursor;
    for (;;) {
        while (p != end && *p && strchr(" \t\r\n\v\f", *p))
            ++p;
        if (end - p < 2 || p[0] != '-' || p[1] != '-')
            break;
        p += 2;
        size_t size = long_open(p, end);
        if (size)
            p = long_end(p, end, size);
        else
            while (p != end && *p != '\n' && *p != '\r')
                ++p;
    }
    const char *begin = p;
    enum token_kind kind = OTHER;
    if (p != end) {
        size_t size = long_open(p, end);
        if (size) {
            p = long_end(p, end, size);
            kind = STRING;
        } else if (*p == '\'' || *p == '"') {
            char quote = *p++;
            while (p != end) {
                if (*p == '\\') {
                    ++p;
                    if (p != end)
                        ++p;
                } else if (*p++ == quote) {
                    break;
                }
            }
            kind = STRING;
        } else if (ascii_alpha(*p)) {
            do { ++p; } while (p != end && (ascii_alpha(*p) || (*p >= '0' && *p <= '9')));
            kind = WORD;
        } else {
            ++p;
        }
    }
    *cursor = p;
    return (struct token){begin, (size_t)(p - begin), kind};
}

static int token_is(struct token t, const char *text)
{
    size_t n = strlen(text);
    return t.length == n && !memcmp(t.text, text, n);
}

static void validate_envelope(lua_State *L, const char *path, const char *source, size_t size)
{
    static const char *const prefix[] = {"LIB", "=", "asdl", ".", "lib", "."};
    static const char *const suffix[] = {"asdl", ".", "signlib", "(", "LIB", ")", "return", "LIB"};
    enum { TAIL = sizeof suffix / sizeof suffix[0] };
    const char *p = source, *end = source + size;
    struct token t, tail[TAIL];
    size_t count = 0;
    for (size_t i = 0; i < sizeof prefix / sizeof prefix[0]; ++i)
        if (!token_is(scan(&p, end), prefix[i]))
            goto invalid_prefix;
    t = scan(&p, end);
    if (!token_is(t, "new") && !token_is(t, "derive"))
        goto invalid_prefix;
    t = scan(&p, end);
    if (token_is(t, "(")) {
        if (scan(&p, end).kind != STRING || !token_is(scan(&p, end), ")"))
            goto invalid_prefix;
    } else if (t.kind != STRING) {
        goto invalid_prefix;
    }
    /* A call suffix would extend the prologue RHS (f "x"(...) or f("x").x). */
    t = scan(&p, end);
    if ((t.kind != WORD && !token_is(t, ";")) ||
        token_is(t, "and") || token_is(t, "or"))
        goto invalid_prefix;
    for (; t.length; t = scan(&p, end))
        if (!token_is(t, ";"))
            tail[count++ % TAIL] = t;
    if (count >= TAIL) {
        size_t i = 0;
        while (i < TAIL && token_is(tail[(count + i) % TAIL], suffix[i]))
            ++i;
        if (i == TAIL)
            return;
    }
    luaL_error(L, "%s: expected library trailer asdl.signlib(LIB); return LIB", path);
    return;
invalid_prefix:
    luaL_error(L, "%s: expected library prologue LIB = asdl.lib.new or asdl.lib.derive with one string", path);
}

static void open_chunk(lua_State *L, const char *path)
{
    luaL_Buffer b;
    size_t total = 0, n;
    FILE *f = fopen(path, "rb");
    if (!f)
        luaL_error(L, "cannot read ASDL library '%s': %s", path, strerror(errno));
    luaL_buffinit(L, &b);
    char data[4096];
    while ((n = fread(data, 1, sizeof data, f)) != 0) {
        total += n;
        if (total > MAX_SOURCE) {
            fclose(f);
            luaL_error(L, "%s: ASDL library source is too large", path);
        }
        luaL_addlstring(&b, data, n);
    }
    int failed = ferror(f);
    fclose(f);
    if (failed)
        luaL_error(L, "cannot finish reading ASDL library '%s'", path);
    luaL_pushresult(&b);
    size_t size;
    const char *source = lua_tolstring(L, -1, &size);
    const char *label = lua_pushfstring(L, "@%s", path);
    if (luaL_loadbufferx(L, source, size, label, "t") != LUA_OK)
        lua_error(L);
    validate_envelope(L, path, source, size);
}

/* Frames are active only around the protected execution of a library.
 * A resolved/parsed file is not in the cache until all checks succeed. */
static int active_frame(lua_State *L)
{
    int stack = installed_table(L, "active");
    lua_rawgeti(L, stack, (lua_Integer)lua_rawlen(L, stack));
    lua_remove(L, stack);
    return lua_gettop(L);
}

/* Weak-key map, indexed by the original export table. */
static int metadata(lua_State *L, int table, int check)
{
    table = lua_absindex(L, table);
    int map = installed_table(L, "metadata");
    lua_pushvalue(L, table);
    lua_rawget(L, map);
    lua_remove(L, map);
    if (check && lua_isnil(L, -1))
        luaL_error(L, "not an ASDL library");
    return lua_gettop(L);
}

static int signed_metadata(lua_State *L, int meta)
{
    lua_getfield(L, meta, "backing");
    int signed_library = lua_istable(L, -1);
    lua_pop(L, 1);
    return signed_library;
}

static int lib_issigned(lua_State *L)
{
    luaL_checkany(L, 1);
    int yes = 0;
    if (lua_istable(L, 1)) {
        int meta = metadata(L, 1, 0);
        yes = lua_istable(L, meta) && signed_metadata(L, meta);
    }
    lua_pushboolean(L, yes);
    return 1;
}

static int make_library(lua_State *L, const char *name, int backing, int parent)
{
    lua_newtable(L);
    int lib = lua_gettop(L);
    if (backing)
        copy_table(L, backing, lib);
    lua_newtable(L);
    int meta = lua_gettop(L);
    lua_pushstring(L, name);
    lua_setfield(L, meta, "name");
    if (parent)
        set_from(L, meta, "parent", parent);
    int frame = active_frame(L);
    if (!lua_isnil(L, frame)) {
        lua_getfield(L, frame, "lib");
        if (!lua_isnil(L, -1))
            luaL_error(L, "a library file must construct exactly one LIB");
        set_from(L, frame, "lib", lib);
        lua_getfield(L, frame, "path");
        lua_setfield(L, meta, "path");
        lua_getfield(L, frame, "dependencies");
        lua_setfield(L, meta, "dependencies");
    } else {
        lua_newtable(L);
        lua_setfield(L, meta, "dependencies");
        if (parent) {
            lua_getfield(L, meta, "dependencies");
            set_from(L, -1, lua_tostring(L, parent), parent);
        }
    }
    int map = installed_table(L, "metadata");
    lua_pushvalue(L, lib);
    lua_pushvalue(L, meta);
    lua_rawset(L, map);
    return lib;
}

static int lib_new(lua_State *L)
{
    lua_pushvalue(L, make_library(L, check_string(L, 1), 0, 0));
    return 1;
}

static int sealed(lua_State *L)
{
    return luaL_error(L, "signed ASDL library is sealed");
}

static int next_export(lua_State *L)
{
    lua_settop(L, 2);
    return lua_next(L, lua_upvalueindex(1)) ? 2 : 0;
}

static int pairs_exports(lua_State *L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushcclosure(L, next_export, 1);
    lua_pushnil(L);
    lua_pushnil(L);
    return 3;
}

static int sign_library(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int meta = metadata(L, 1, 1);
    if (signed_metadata(L, meta))
        return luaL_error(L, "ASDL library is already signed");
    if (lua_getmetatable(L, 1))
        return luaL_error(L, "an unsigned ASDL library cannot have a metatable");
    lua_newtable(L);
    int backing = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING || lua_rawlen(L, -2) == 0)
            return luaL_error(L, "invalid ASDL library export: keys must be nonempty strings");
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, backing);
    }
    lua_pushnil(L);
    while (lua_next(L, backing)) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_pushnil(L);
        lua_rawset(L, 1);
    }
    set_from(L, meta, "backing", backing);
    lua_newtable(L);
    set_from(L, -1, "__index", backing);
    lua_pushcfunction(L, sealed);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, backing);
    lua_pushcclosure(L, pairs_exports, 1);
    lua_setfield(L, -2, "__pairs");
    lua_pushliteral(L, "signed ASDL library");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

static int safe_getmetatable(lua_State *L)
{
    luaL_checkany(L, 1);
    /* Primitive/host userdata metatables belong to the entire Lua state. */
    if (lua_istable(L, 1) && lua_getmetatable(L, 1)) {
        if (luaL_getmetafield(L, 1, "__metatable") == LUA_TNIL)
            lua_pushvalue(L, -1);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int module_environment(lua_State *L)
{
    static const char *const globals[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal",
        "rawget", "rawlen", "select", "setmetatable", "tonumber", "tostring",
        "type", "xpcall", "_VERSION"
    };
    static const char *const modules[] = {"math", "string", "table", "utf8"};
    lua_newtable(L);
    int env = lua_gettop(L);
    set_from(L, env, "_G", env);
    lua_pushcfunction(L, safe_getmetatable);
    lua_setfield(L, env, "getmetatable");
    for (size_t i = 0; i < sizeof globals / sizeof globals[0]; ++i) {
        lua_getglobal(L, globals[i]);
        lua_setfield(L, env, globals[i]);
    }
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; ++i) {
        lua_getglobal(L, modules[i]);
        if (lua_istable(L, -1)) {
            lua_newtable(L);
            copy_table(L, -2, -1);
            lua_setfield(L, env, modules[i]);
        }
        lua_pop(L, 1);
    }
    int host = installed_table(L, "host");
    lua_newtable(L);
    int asdl = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, host)) {
        lua_pushvalue(L, -2);
        if (lua_istable(L, -2)) {
            lua_newtable(L);
            copy_table(L, -3, -1);
        } else {
            lua_pushvalue(L, -2);
        }
        lua_rawset(L, asdl);
        lua_pop(L, 1);
    }
    lua_setfield(L, env, "asdl");
    lua_pop(L, 1); /* host */
    return env;
}

static int sorted_keys(lua_State *L, int table)
{
    lua_newtable(L);
    int result = lua_gettop(L);
    lua_Integer i = 0;
    lua_pushnil(L);
    while (lua_next(L, table)) {
        lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_rawseti(L, result, ++i);
    }
    if (i > 1) {
        lua_getfield(L, CTX, "sort");
        lua_pushvalue(L, result);
        lua_call(L, 1, 0);
    }
    return result;
}

static void report_cycle(lua_State *L, int stack, lua_Integer first,
                         lua_Integer count, const char *path)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "circular ASDL library dependency: ");
    for (lua_Integer i = first; i <= count; ++i) {
        lua_rawgeti(L, stack, i);
        lua_getfield(L, -1, "path");
        luaL_addstring(&b, lua_tostring(L, -1));
        luaL_addstring(&b, " -> ");
        lua_pop(L, 2);
    }
    luaL_addstring(&b, path);
    luaL_pushresult(&b);
    lua_error(L);
}

static int load_library(lua_State *L, const char *name, const char *version, int *path_index)
{
    luaL_checkstack(L, 48, "loading ASDL library");
    resolve(L, name);
    int path = *path_index = lua_gettop(L);
    const char *filename = lua_tostring(L, path);
    int cache = installed_table(L, "cache");
    int stack = installed_table(L, "active");
    lua_Integer depth = (lua_Integer)lua_rawlen(L, stack);
    lua_pushvalue(L, path);
    lua_rawget(L, cache);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        for (lua_Integer i = 1; i <= depth; ++i) {
            lua_rawgeti(L, stack, i);
            lua_getfield(L, -1, "path");
            if (lua_rawequal(L, path, -1))
                report_cycle(L, stack, i, depth, filename);
            lua_pop(L, 2);
        }
        /* Parse before installing the active frame (the parser can fail). */
        open_chunk(L, filename);
        int chunk = lua_gettop(L);
        int env = module_environment(L);
        lua_pushvalue(L, env);
        lua_setupvalue(L, chunk, 1);
        lua_newtable(L);
        int frame = lua_gettop(L);
        set_from(L, frame, "path", path);
        lua_newtable(L);
        lua_setfield(L, frame, "dependencies");
        lua_pushvalue(L, frame);
        lua_rawseti(L, stack, depth + 1);
        lua_pushvalue(L, chunk);
        int status = lua_pcall(L, 0, 1, 0);
        lua_pushnil(L);
        lua_rawseti(L, stack, depth + 1);
        if (status != LUA_OK)
            lua_error(L);
        int lib = lua_gettop(L);
        lua_getfield(L, frame, "lib");
        lua_pushliteral(L, "LIB");
        lua_rawget(L, env);
        if (!lua_istable(L, lib) || !lua_rawequal(L, lib, -1) || !lua_rawequal(L, lib, -2))
            luaL_error(L, "%s: library must return its original LIB", filename);
        lua_pop(L, 2);
        int meta = metadata(L, lib, 1);
        if (!signed_metadata(L, meta))
            luaL_error(L, "%s: library must return signed LIB", filename);
        lua_pop(L, 1);
        lua_pushvalue(L, path);
        lua_pushvalue(L, lib);
        lua_rawset(L, cache);
    }
    int lib = lua_gettop(L);
    if (version) {
        int meta = metadata(L, lib, 1);
        lua_getfield(L, meta, "version");
        const char *actual = lua_tostring(L, -1);
        if (!actual || strcmp(actual, version))
            luaL_error(L, "%s: ASDL library version mismatch: expected '%s', found '%s'",
                       filename, version, actual ? actual : "(undeclared)");
        lua_pop(L, 2);
    }
    if (depth) {
        lua_rawgeti(L, stack, depth);
        lua_getfield(L, -1, "dependencies");
        lua_pushvalue(L, path);
        lua_pushboolean(L, 1);
        lua_rawset(L, -3);
        lua_pop(L, 2);
    }
    return lib;
}

static int lib_require(lua_State *L)
{
    const char *name = check_string(L, 1);
    const char *version = lua_isnoneornil(L, 2) ? NULL : check_string(L, 2);
    int path;
    lua_pushvalue(L, load_library(L, name, version, &path));
    return 1;
}

static int lib_derive(lua_State *L)
{
    const char *name = check_string(L, 1);
    int path;
    int lib = load_library(L, name, NULL, &path);
    int meta = metadata(L, lib, 1);
    lua_getfield(L, meta, "backing");
    int backing = lua_gettop(L);
    const char *derived = lua_pushfstring(L, "%s:derived", name);
    lua_pushvalue(L, make_library(L, derived, backing, path));
    return 1;
}

static int lib_version(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int count = lua_gettop(L);
    int meta = metadata(L, 1, 1);
    lua_getfield(L, meta, "version");
    if (count == 1)
        return 1;
    check_string(L, 2);
    if (signed_metadata(L, meta))
        return luaL_error(L, "cannot version a signed ASDL library");
    if (!lua_isnil(L, -1))
        return luaL_error(L, "ASDL library version already declared");
    set_from(L, meta, "version", 2);
    return 0;
}

static int lib_loaded(lua_State *L)
{
    sorted_keys(L, installed_table(L, "cache"));
    return 1;
}

static int export_names(lua_State *L, int lib, int meta)
{
    lua_getfield(L, meta, "backing");
    if (lua_istable(L, -1))
        return sorted_keys(L, lua_gettop(L));
    /* Unsigned tables may temporarily contain keys signing will reject. */
    lua_newtable(L);
    int keys = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, lib)) {
        lua_pop(L, 1);
        if (lua_type(L, -1) == LUA_TSTRING && lua_rawlen(L, -1)) {
            lua_pushvalue(L, -1);
            lua_pushboolean(L, 1);
            lua_rawset(L, keys);
        }
    }
    return sorted_keys(L, keys);
}

static int lib_exports(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    export_names(L, 1, metadata(L, 1, 1));
    return 1;
}

static int lib_describe(lua_State *L)
{
    static const char *const fields[] = {"name", "path", "parent", "version"};
    luaL_checktype(L, 1, LUA_TTABLE);
    int meta = metadata(L, 1, 1);
    lua_newtable(L);
    int result = lua_gettop(L);
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        lua_getfield(L, meta, fields[i]);
        lua_setfield(L, result, fields[i]);
    }
    lua_pushboolean(L, signed_metadata(L, meta));
    lua_setfield(L, result, "signed");
    set_from(L, result, "exports", export_names(L, 1, meta));
    lua_getfield(L, meta, "dependencies");
    set_from(L, result, "dependencies", sorted_keys(L, lua_gettop(L)));
    lua_pushvalue(L, result);
    return 1;
}

int asdl_loader_install(lua_State *L, int asdl_index, const char *stdlib_root)
{
    static const luaL_Reg functions[] = {
        {"new", lib_new}, {"derive", lib_derive}, {"require", lib_require},
        {"resolve", lib_resolve}, {"searchpath", lib_searchpath},
        {"exports", lib_exports}, {"describe", lib_describe}, {"loaded", lib_loaded},
        {"issigned", lib_issigned}, {"version", lib_version}, {NULL, NULL}
    };
    int top = lua_gettop(L);
    asdl_index = lua_absindex(L, asdl_index);
    luaL_checktype(L, asdl_index, LUA_TTABLE);
    lua_newtable(L);
    int context = lua_gettop(L);
    set_from(L, context, "host", asdl_index);
    lua_getglobal(L, "table");
    lua_getfield(L, -1, "sort");
    if (!lua_isfunction(L, -1))
        return luaL_error(L, "ASDL loader requires the Lua table library");
    lua_setfield(L, context, "sort");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_setfield(L, context, "active");
    lua_newtable(L);
    lua_setfield(L, context, "cache");
    lua_newtable(L);
    lua_newtable(L);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, context, "metadata");

    lua_newtable(L);
    int roots = lua_gettop(L);
    lua_Integer i = 0;
    const char *env = getenv("LIBSTONE_ASDL_PATH");
    while (env && *env) {
        const char *next = strchr(env, ':');
        size_t size = next ? (size_t)(next - env) : strlen(env);
        if (size) {
            lua_pushlstring(L, env, size);
            lua_rawseti(L, roots, ++i);
        }
        env = next ? next + 1 : NULL;
    }
#ifdef LIBSTONE_ASDL_STDLIB_DIR
    if (!stdlib_root)
        stdlib_root = LIBSTONE_ASDL_STDLIB_DIR;
#endif
    if (stdlib_root && *stdlib_root) {
        lua_pushstring(L, stdlib_root);
        lua_rawseti(L, roots, ++i);
    }
    lua_setfield(L, context, "roots");

    luaL_newlibtable(L, functions);
    lua_pushvalue(L, context);
    luaL_setfuncs(L, functions, 1);
    lua_setfield(L, asdl_index, "lib");
    lua_pushvalue(L, context);
    lua_pushcclosure(L, sign_library, 1);
    lua_setfield(L, asdl_index, "signlib");
    lua_settop(L, top);
    return 0;
}
