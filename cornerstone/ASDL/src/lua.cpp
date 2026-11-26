#include "asdl/lua.hpp"
#include "asdl/schema.hpp"

#include <kaguya/compatibility.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace asdl {
namespace {

constexpr char node_mt[] = "asdl.node";
enum Slot { fields_slot = 1, annotations_slot, location_slot };

struct Stack {
    lua_State* lua;
    int top;
    explicit Stack(lua_State* l) : lua(l), top(lua_gettop(l)) {}
    ~Stack() { lua_settop(lua, top); }
};

void room(lua_State* l, int n = 24) {
    if (!lua_checkstack(l, n)) throw std::runtime_error("ASDL Lua stack limit exceeded");
}

[[noreturn]] void fail(std::string message) { throw std::runtime_error(std::move(message)); }

std::string_view text(lua_State* l, int i) {
    if (lua_type(l, i) != LUA_TSTRING) fail("expected a string");
    std::size_t n;
    const char* data = lua_tolstring(l, i, &n);
    return {data, n};
}

void push(lua_State* l, std::string_view s) {
    lua_pushlstring(l, s.data() ? s.data() : "", s.size());
}

int field(lua_State* l, int i, std::string_view key) {
    i = lua_absindex(l, i);
    push(l, key);
    return kaguya::compat::lua_rawget_rtype(l, i);
}

void put(lua_State* l, int i, std::string_view key) {
    i = lua_absindex(l, i);
    push(l, key);
    lua_insert(l, -2);
    lua_rawset(l, i);
}

void set(lua_State* l, const char* key, std::string_view v) {
    push(l, v);
    lua_setfield(l, -2, key);
}

void integer(lua_State* l, const char* key, std::size_t v) {
    lua_pushinteger(l, static_cast<lua_Integer>(v));
    lua_setfield(l, -2, key);
}

bool sequence(const Field& f) { return f.qualifier == '*' || f.qualifier == '+'; }

std::size_t dense(lua_State* l, int i) {
    if (lua_type(l, i) != LUA_TTABLE) fail("expected a dense sequence");
    i = lua_absindex(l, i);
    const auto n = lua_rawlen(l, i);
    std::size_t count = 0;
    lua_pushnil(l);
    while (lua_next(l, i)) {
        const auto key = lua_tointeger(l, -2);
        if (!lua_isinteger(l, -2) || key < 1 || static_cast<lua_Unsigned>(key) > n)
            fail("expected a dense sequence");
        ++count;
        lua_pop(l, 1);
    }
    if (count != n) fail("expected a dense sequence");
    return n;
}

int traceback(lua_State* l) {
    const char* s = lua_type(l, 1) == LUA_TSTRING ? lua_tostring(l, 1) : "non-string Lua error";
    luaL_traceback(l, l, s, 1);
    return 1;
}

void require_ok(lua_State* l, int status) {
    if (status == LUA_OK) return;
    if (lua_type(l, -1) == LUA_TSTRING) fail(std::string(text(l, -1)));
    fail("Lua execution failed (status " + std::to_string(status) + ")");
}

void source_pos(lua_State* l, const SourcePos& p) {
    lua_newtable(l);
    integer(l, "offset", p.offset);
    integer(l, "line", p.line);
    integer(l, "column", p.column);
}

void source_span(lua_State* l, const SourceSpan& span) {
    lua_newtable(l);
    set(l, "file", span.file);
    source_pos(l, span.begin);
    lua_setfield(l, -2, "begin");
    source_pos(l, span.end);
    lua_setfield(l, -2, "end");
}

// Copy plain location records (never return mutable internal metadata).
void copy_location(lua_State* l, int i, int depth = 0) {
    i = lua_absindex(l, i);
    if (lua_isnoneornil(l, i)) {
        lua_pushnil(l);
        return;
    }
    if (depth == 4 || lua_type(l, i) != LUA_TTABLE) fail("invalid source location");
    room(l);
    lua_newtable(l);
    const int out = lua_gettop(l);
    lua_pushnil(l);
    while (lua_next(l, i)) {
        if (lua_type(l, -2) != LUA_TSTRING) fail("invalid source location key");
        lua_pushvalue(l, -2);
        switch (lua_type(l, -2)) {
        case LUA_TNUMBER: case LUA_TSTRING: case LUA_TBOOLEAN: lua_pushvalue(l, -2); break;
        case LUA_TTABLE: copy_location(l, -2, depth + 1); break;
        default: fail("invalid source location value");
        }
        lua_rawset(l, out);
        lua_pop(l, 1);
    }
}

} // namespace

struct LuaSession::Impl {
    struct Shape {
        const Definition* def;
        const Constructor* ctor;
        std::vector<const Field*> fields;
    };
    struct Node {
        Impl* owner;
        const Shape* shape;
        lua_Integer id;
        lua_Integer origin;
        bool frozen;
    };

    Schema schema;
    LuaOptions options;
    std::map<std::string, std::vector<Shape>, std::less<>> shapes;
    std::map<std::string, std::size_t, std::less<>> diagnostics;
    lua_State* lua = nullptr;
    std::size_t allocated = 0;
    std::size_t remaining = 0;
    int quantum = 0;
    bool exhausted = false;
    bool executing = false;
    lua_Integer last_id = 0;

    explicit Impl(const Schema& s, LuaOptions o) : schema(s), options(std::move(o)) {
        // Bound native recursion independently of Lua's VM stack.
        if (options.traversal_limit == 0 || options.traversal_limit > 512)
            fail("ASDL traversal_limit must be between 1 and 512");
        for (const auto& def : schema.definitions) {
            auto& family = shapes[def.name];
            auto add = [&](const Constructor* ctor) {
                Shape shape{&def, ctor, {}};
                for (const auto& f : ctor ? ctor->fields : def.fields) shape.fields.push_back(&f);
                for (const auto& f : def.attributes) shape.fields.push_back(&f);
                family.push_back(std::move(shape));
            };
            if (def.kind == DefinitionKind::product) add(nullptr);
            else for (const auto& ctor : def.constructors) add(&ctor);
        }
    }

    ~Impl() { if (lua) lua_close(lua); }

    static Impl& self(lua_State* l) {
        return **static_cast<Impl**>(lua_getextraspace(l));
    }

    static void* alloc(void* context, void* ptr, std::size_t prev, std::size_t next) {
        auto& s = *static_cast<Impl*>(context);
        if (!ptr) prev = 0; // Lua passes a type tag for new objects.
        if (!next) {
            std::free(ptr);
            s.allocated -= prev;
            return nullptr;
        }
        const auto base = s.allocated - prev;
        if (next > std::numeric_limits<std::size_t>::max() - base ||
            (s.options.memory_limit && next > prev && base + next > s.options.memory_limit))
            return nullptr;
        if (void* result = std::realloc(ptr, next)) {
            s.allocated = base + next;
            return result;
        }
        return nullptr;
    }

    static void budget(lua_State* l, lua_Debug*) {
        auto& s = self(l);
        if (s.remaining <= static_cast<std::size_t>(s.quantum)) {
            s.exhausted = true;
            lua_sethook(l, budget, LUA_MASKCOUNT, 1);
            luaL_error(l, "ASDL Lua instruction limit exceeded");
            return;
        }
        s.remaining -= s.quantum;
        if (s.remaining < static_cast<std::size_t>(s.quantum)) {
            s.quantum = static_cast<int>(s.remaining);
            lua_sethook(l, budget, LUA_MASKCOUNT, s.quantum);
        }
    }

    struct Execution {
        Impl& s;
        explicit Execution(Impl& state) : s(state) {
            if (s.executing) fail("reentrant LuaSession execution");
            s.executing = true;
            s.exhausted = false;
            s.remaining = s.options.instruction_limit;
            if (s.remaining) {
                s.quantum = static_cast<int>(std::min<std::size_t>(s.remaining, 1000));
                lua_sethook(s.lua, budget, LUA_MASKCOUNT, s.quantum);
            }
        }
        ~Execution() {
            lua_sethook(s.lua, nullptr, 0, 0);
            s.executing = false;
        }
    };

    template<int (Impl::*Method)(lua_State*)>
    static int invoke(lua_State* l) {
        try {
            return (self(l).*Method)(l);
        } catch (const std::exception& e) {
            lua_pushstring(l, e.what());
        }
        // No C++ exception object/local needs destruction after lua_error.
        return lua_error(l);
    }

    const Node* test(lua_State* l, int i) const {
        if (lua_type(l, i) != LUA_TUSERDATA || lua_rawlen(l, i) != sizeof(Node)) return nullptr;
        const auto* n = static_cast<Node*>(luaL_testudata(l, i, node_mt));
        return n && n->owner == this && n->shape && n->id > 0 ? n : nullptr;
    }

    const Node& node(lua_State* l, int i) const {
        if (const auto* n = test(l, i)) return *n;
        fail("expected a live node from this ASDL arena");
    }

    void depth(std::size_t d) const {
        if (d >= options.traversal_limit) fail("ASDL traversal limit exceeded");
    }

    const Shape& shape(std::string_view type, std::optional<std::string_view> tag) const {
        auto it = shapes.find(type);
        if (it == shapes.end()) fail("unknown ASDL type: " + std::string(type));
        for (const auto& s : it->second)
            if ((!s.ctor && !tag) || (s.ctor && tag && s.ctor->name == *tag)) return s;
        fail("invalid constructor for ASDL type: " + std::string(type));
    }

    static const Field& declared(const Shape& s, std::string_view name) {
        for (const auto* f : s.fields) if (f->name == name) return *f;
        fail("unknown AST field: " + std::string(name));
    }

    void fresh(lua_State* l, const Shape& s, int data, int origin) {
        data = lua_absindex(l, data);
        origin = lua_absindex(l, origin);
        const auto* old = test(l, origin);
        if (last_id == std::numeric_limits<lua_Integer>::max()) fail("ASDL node IDs exhausted");
        const auto id = ++last_id;
        // Node is trivially destructible; Lua owns the struct and all uservalues.
        void* bytes = lua_newuserdatauv(l, sizeof(Node), 3);
        new (bytes) Node{this, &s, id, old ? old->id : 0, false};
        const int result = lua_gettop(l);
        luaL_setmetatable(l, node_mt);
        lua_pushvalue(l, data);
        lua_setiuservalue(l, result, fields_slot);
        if (old) lua_getiuservalue(l, origin, location_slot);
        else if (origin > 0 && origin < result) copy_location(l, origin);
        else lua_pushnil(l);
        lua_setiuservalue(l, result, location_slot);
    }

    void normalized(lua_State* l, int i, std::string_view type) {
        if (shapes.count(type)) {
            if (node(l, i).shape->def->name != type)
                fail("expected node of ASDL type " + std::string(type));
        } else if (type == "int" || type == "size" || type == "uint" || type == "usize") {
            if (!lua_isinteger(l, i) || ((type == "uint" || type == "usize") && lua_tointeger(l, i) < 0))
                fail("expected " + std::string(type) + " integer");
        } else if (type == "real") {
            if (lua_type(l, i) != LUA_TNUMBER || !std::isfinite(lua_tonumber(l, i)))
                fail("expected a finite real number");
            const auto n = lua_tonumber(l, i);
            lua_pushnumber(l, n == 0 ? 0 : n); // canonicalize negative zero
            return;
        } else if (type == "bool") {
            if (!lua_isboolean(l, i)) fail("expected a boolean");
        } else if (type == "string" || type == "identifier" || type == "path" ||
                   type == "buffer" || type == "regexp" || type == "sexpr") {
            if (lua_type(l, i) != LUA_TSTRING) fail("expected " + std::string(type) + " string");
        } else {
            fail("unsupported Lua value type: " + std::string(type));
        }
        lua_pushvalue(l, i);
    }

    void normalized_field(lua_State* l, int i, const Field& f) {
        i = lua_absindex(l, i);
        if (sequence(f)) {
            const auto n = lua_isnoneornil(l, i) ? 0 : dense(l, i);
            if (n == 0 && f.qualifier == '+') fail("nonempty sequence required for field " + f.name);
            lua_newtable(l);
            const int out = lua_gettop(l);
            for (std::size_t j = 1; j <= n; ++j) {
                lua_rawgeti(l, i, static_cast<lua_Integer>(j));
                normalized(l, -1, f.type);
                lua_rawseti(l, out, static_cast<lua_Integer>(j));
                lua_pop(l, 1);
            }
        } else if (lua_isnoneornil(l, i)) {
            if (f.qualifier != '?') fail("missing required field " + f.name);
            lua_pushnil(l);
        } else {
            normalized(l, i, f.type);
        }
    }

    static void clone_sequence(lua_State* l, int i) {
        i = lua_absindex(l, i);
        lua_newtable(l);
        const int out = lua_gettop(l);
        const auto n = static_cast<lua_Integer>(lua_rawlen(l, i));
        for (lua_Integer j = 1; j <= n; ++j) {
            lua_rawgeti(l, i, j);
            lua_rawseti(l, out, j);
        }
    }

    static void value(lua_State* l, int data, const Field& f) {
        field(l, data, f.name);
        if (sequence(f)) {
            clone_sequence(l, -1);
            lua_remove(l, -2);
        }
    }

    int make(lua_State* l) {
        const auto& s = shape(text(l, 1), lua_isnoneornil(l, 2)
            ? std::nullopt : std::optional(text(l, 2)));
        const int argc = lua_gettop(l);
        if (!lua_isnoneornil(l, 3) && !lua_istable(l, 3)) fail("expected a named AST field mapping");
        if (lua_istable(l, 3)) {
            lua_pushnil(l);
            while (lua_next(l, 3)) {
                declared(s, text(l, -2));
                lua_pop(l, 1);
            }
        }
        lua_newtable(l);
        const int out = lua_gettop(l);
        for (const auto* f : s.fields) {
            if (argc >= 3 && lua_istable(l, 3)) field(l, 3, f->name);
            else lua_pushnil(l);
            normalized_field(l, -1, *f);
            put(l, out, f->name);
            lua_pop(l, 1);
        }
        fresh(l, s, out, argc >= 4 ? 4 : 0);
        return 1;
    }

    int construct(lua_State* l) {
        const auto& s = shape(text(l, 1), lua_isnoneornil(l, 2)
            ? std::nullopt : std::optional(text(l, 2)));
        const int argc = lua_gettop(l);
        if (argc > 2 && static_cast<std::size_t>(argc - 2) > s.fields.size())
            fail("too many positional AST fields");
        lua_newtable(l);
        const int out = lua_gettop(l);
        for (std::size_t i = 0; i < s.fields.size(); ++i) {
            const auto& f = *s.fields[i];
            const int arg = static_cast<int>(i) + 3;
            if (arg > argc) lua_pushnil(l);
            normalized_field(l, arg > argc ? -1 : arg, f);
            put(l, out, f.name);
            if (arg > argc) lua_pop(l, 1);
        }
        fresh(l, s, out, 0);
        return 1;
    }

    int isnode(lua_State* l) { lua_pushboolean(l, test(l, 1) != nullptr); return 1; }
    int type(lua_State* l) { push(l, node(l, 1).shape->def->name); return 1; }
    int tag(lua_State* l) {
        const auto* ctor = node(l, 1).shape->ctor;
        if (ctor) push(l, ctor->name);
        else lua_pushnil(l);
        return 1;
    }
    int id(lua_State* l) { lua_pushinteger(l, node(l, 1).id); return 1; }
    int same(lua_State* l) {
        const auto* a = test(l, 1);
        lua_pushboolean(l, a && a == test(l, 2));
        return 1;
    }
    int kind(lua_State* l) { push(l, node(l, 1).shape->ctor ? "sum" : "product"); return 1; }
    int is_sum(lua_State* l) {
        const auto* n = test(l, 1);
        lua_pushboolean(l, n && n->shape->ctor);
        return 1;
    }
    int is_product(lua_State* l) {
        const auto* n = test(l, 1);
        lua_pushboolean(l, n && !n->shape->ctor);
        return 1;
    }
    int is_enum(lua_State* l) {
        const auto* n = test(l, 1);
        lua_pushboolean(l, n && n->shape->def->is_enum);
        return 1;
    }
    int get(lua_State* l) {
        const auto& f = declared(*node(l, 1).shape, text(l, 2));
        lua_getiuservalue(l, 1, fields_slot);
        value(l, -1, f);
        return 1;
    }
    int has(lua_State* l) {
        const auto& s = *node(l, 1).shape;
        const auto name = text(l, 2);
        bool present = false;
        for (const auto* f : s.fields) if (f->name == name) {
            lua_getiuservalue(l, 1, fields_slot);
            present = field(l, -1, name) != LUA_TNIL;
            break;
        }
        lua_pushboolean(l, present);
        return 1;
    }
    static void field_descriptor(lua_State* l, const Field& f, bool attribute = false) {
        lua_newtable(l);
        set(l, "name", f.name);
        set(l, "type", f.type);
        const char cardinality = f.qualifier ? f.qualifier : '1';
        set(l, "cardinality", std::string_view(&cardinality, 1));
        lua_pushboolean(l, attribute);
        lua_setfield(l, -2, "attribute");
        if (!f.refinement.empty()) set(l, "refinement", f.refinement);
        source_span(l, f.span);
        lua_setfield(l, -2, "span");
    }
    int fields(lua_State* l) {
        const auto& s = *node(l, 1).shape;
        lua_getiuservalue(l, 1, fields_slot);
        const int data = lua_gettop(l);
        lua_newtable(l);
        const int out = lua_gettop(l);
        lua_Integer index = 0;
        const auto ordinary = s.ctor ? s.ctor->fields.size() : s.def->fields.size();
        for (const auto* f : s.fields) {
            field_descriptor(l, *f, static_cast<std::size_t>(index) >= ordinary);
            value(l, data, *f);
            lua_setfield(l, -2, "value");
            lua_rawseti(l, out, ++index);
        }
        return 1;
    }
    int destruct(lua_State* l) {
        const auto& s = *node(l, 1).shape;
        lua_getiuservalue(l, 1, fields_slot);
        const int data = lua_gettop(l);
        if (s.fields.size() > static_cast<std::size_t>(std::numeric_limits<int>::max() - 4))
            fail("too many fields to destruct");
        room(l, static_cast<int>(s.fields.size()) + 4);
        for (const auto* f : s.fields) value(l, data, *f);
        return static_cast<int>(s.fields.size());
    }

    // Visit scalar field values (including each element of qualified fields).
    // The callback sees the value at the stack top and must leave it there.
    template<class Visitor>
    void each(lua_State* l, int index, Visitor visit) {
        const auto& n = node(l, index);
        index = lua_absindex(l, index);
        lua_getiuservalue(l, index, fields_slot);
        const int data = lua_gettop(l);
        for (const auto* f : n.shape->fields) {
            field(l, data, f->name);
            if (sequence(*f)) {
                const auto count = static_cast<lua_Integer>(lua_rawlen(l, -1));
                for (lua_Integer j = 1; j <= count; ++j) {
                    lua_rawgeti(l, -1, j);
                    visit();
                    lua_pop(l, 1);
                }
            } else visit();
            lua_pop(l, 1);
        }
        lua_pop(l, 1);
    }

    int children(lua_State* l) {
        node(l, 1);
        lua_newtable(l);
        const int out = lua_gettop(l);
        lua_Integer n = 0;
        each(l, 1, [&] {
            if (test(l, -1)) {
                lua_pushvalue(l, -1);
                lua_rawseti(l, out, ++n);
            }
        });
        return 1;
    }

    int copy(lua_State* l) {
        const auto& n = node(l, 1);
        // Always swap whole field maps on writes; sharing an old map is safe.
        lua_getiuservalue(l, 1, fields_slot);
        fresh(l, *n.shape, -1, 1);
        return 1;
    }

    void clone_tree(lua_State* l, int i, int memo, std::size_t d) {
        i = lua_absindex(l, i);
        const auto* n = test(l, i);
        if (!n) { lua_pushvalue(l, i); return; }
        depth(d);
        room(l);
        if (lua_rawgeti(l, memo, n->id) != LUA_TNIL) return;
        lua_pop(l, 1);
        lua_getiuservalue(l, i, fields_slot);
        const int data = lua_gettop(l);
        lua_newtable(l);
        const int out = lua_gettop(l);
        for (const auto* f : n->shape->fields) {
            field(l, data, f->name);
            if (!sequence(*f)) {
                clone_tree(l, -1, memo, d + 1);
            } else {
                const int seq = lua_gettop(l);
                lua_newtable(l);
                const int dest = lua_gettop(l);
                const auto len = static_cast<lua_Integer>(lua_rawlen(l, seq));
                for (lua_Integer j = 1; j <= len; ++j) {
                    lua_rawgeti(l, seq, j);
                    clone_tree(l, -1, memo, d + 1);
                    lua_rawseti(l, dest, j);
                    lua_pop(l, 1);
                }
            }
            put(l, out, f->name);
            lua_pop(l, 1);
        }
        fresh(l, *n->shape, out, i);
        lua_pushvalue(l, -1);
        lua_rawseti(l, memo, n->id);
        lua_remove(l, out);
        lua_remove(l, data);
    }
    int clone(lua_State* l) {
        node(l, 1);
        lua_newtable(l);
        clone_tree(l, 1, lua_gettop(l), 0);
        return 1;
    }

    void collect(lua_State* l, int i, std::vector<Node*>& nodes,
                 std::unordered_set<const Node*>& seen, std::size_t d,
                 const Node* forbidden = nullptr) {
        const auto* n = test(l, i);
        if (!n) return;
        if (n == forbidden) fail("ASDL mutation would create a cycle");
        depth(d);
        if (!seen.insert(n).second) return;
        room(l);
        nodes.push_back(const_cast<Node*>(n));
        each(l, i, [&] { collect(l, -1, nodes, seen, d + 1, forbidden); });
    }

    int update(lua_State* l, bool in_place) {
        const auto& n = node(l, 1);
        if (in_place && n.frozen) fail("cannot mutate a frozen AST node");
        const auto& f = declared(*n.shape, text(l, 2));
        if (lua_gettop(l) < 3) lua_settop(l, 3);
        normalized_field(l, 3, f);
        const int incoming = lua_gettop(l);
        if (in_place) {
            std::vector<Node*> nodes;
            std::unordered_set<const Node*> seen;
            if (sequence(f)) {
                const auto count = static_cast<lua_Integer>(lua_rawlen(l, incoming));
                for (lua_Integer i = 1; i <= count; ++i) {
                    lua_rawgeti(l, incoming, i);
                    collect(l, -1, nodes, seen, 0, &n);
                    lua_pop(l, 1);
                }
            } else collect(l, incoming, nodes, seen, 0, &n);
        }
        lua_getiuservalue(l, 1, fields_slot);
        const int data = lua_gettop(l);
        lua_newtable(l);
        const int out = lua_gettop(l);
        for (const auto* item : n.shape->fields) {
            if (item == &f) lua_pushvalue(l, incoming);
            else field(l, data, item->name);
            put(l, out, item->name);
        }
        if (in_place) {
            lua_setiuservalue(l, 1, fields_slot);
            lua_pushvalue(l, 1);
        } else fresh(l, *n.shape, out, 1);
        return 1;
    }
    int assign(lua_State* l) { return update(l, true); }
    int replace(lua_State* l) { return update(l, false); }

    bool map_value(lua_State* l, const Field& f) {
        // Source scalar is at the top; leave replacement above it.
        if (!test(l, -1)) { lua_pushvalue(l, -1); return false; }
        const int source = lua_gettop(l);
        lua_pushcfunction(l, traceback);
        const int handler = lua_gettop(l);
        lua_pushvalue(l, 2);
        lua_pushvalue(l, source);
        const int status = lua_pcall(l, 1, 1, handler);
        lua_remove(l, handler);
        if (status != LUA_OK) {
            lua_error(l); // Retain the callback's traceback and error status message.
            return false;
        }
        if (lua_isnil(l, -1)) {
            lua_pop(l, 1);
            lua_pushvalue(l, -1);
            return false;
        }
        if (node(l, -1).shape->def->name != f.type) fail("map_children returned a different AST type");
        return !lua_rawequal(l, -1, -2);
    }
    int map_children(lua_State* l) {
        const auto& n = node(l, 1);
        if (!lua_isfunction(l, 2)) fail("map_children requires a function");
        lua_getiuservalue(l, 1, fields_slot);
        const int data = lua_gettop(l);
        lua_newtable(l);
        const int out = lua_gettop(l);
        bool changed = false;
        for (const auto* f : n.shape->fields) {
            field(l, data, f->name);
            if (!sequence(*f)) {
                changed = map_value(l, *f) || changed;
            } else {
                const int seq = lua_gettop(l);
                lua_newtable(l);
                const int dest = lua_gettop(l);
                const auto len = static_cast<lua_Integer>(lua_rawlen(l, seq));
                for (lua_Integer j = 1; j <= len; ++j) {
                    lua_rawgeti(l, seq, j);
                    changed = map_value(l, *f) || changed;
                    lua_rawseti(l, dest, j);
                    lua_pop(l, 1);
                }
            }
            put(l, out, f->name);
            lua_pop(l, 1);
        }
        if (changed) fresh(l, *n.shape, out, 1);
        else lua_pushvalue(l, 1);
        return 1;
    }

    int validate(lua_State* l) {
        // All structural write paths enforce declarations and acyclicity.
        std::vector<Node*> nodes;
        std::unordered_set<const Node*> seen;
        node(l, 1);
        collect(l, 1, nodes, seen, 0);
        lua_pushboolean(l, true);
        return 1;
    }
    int freeze(lua_State* l) {
        std::vector<Node*> nodes;
        std::unordered_set<const Node*> seen;
        node(l, 1);
        collect(l, 1, nodes, seen, 0);
        for (auto* n : nodes) n->frozen = true;
        lua_pushvalue(l, 1);
        return 1;
    }
    int frozen(lua_State* l) { lua_pushboolean(l, node(l, 1).frozen); return 1; }

    using Pairs = std::set<std::pair<lua_Integer, lua_Integer>>;
    bool equal_value(lua_State* l, int a, int b, Pairs& seen, std::size_t d) {
        if (lua_rawequal(l, a, b)) return true;
        const auto* lhs = test(l, a);
        const auto* rhs = test(l, b);
        if (!lhs || !rhs || lhs->shape != rhs->shape) return false;
        depth(d);
        room(l);
        if (!seen.emplace(lhs->id, rhs->id).second) return true;
        Stack stack(l);
        a = lua_absindex(l, a);
        b = lua_absindex(l, b);
        lua_getiuservalue(l, a, fields_slot);
        const int left = lua_gettop(l);
        lua_getiuservalue(l, b, fields_slot);
        const int right = lua_gettop(l);
        for (const auto* f : lhs->shape->fields) {
            field(l, left, f->name);
            field(l, right, f->name);
            if (sequence(*f)) {
                const int x = lua_gettop(l) - 1, y = x + 1;
                const auto len = static_cast<lua_Integer>(lua_rawlen(l, x));
                if (static_cast<std::size_t>(len) != lua_rawlen(l, y)) return false;
                for (lua_Integer j = 1; j <= len; ++j) {
                    lua_rawgeti(l, x, j);
                    lua_rawgeti(l, y, j);
                    if (!equal_value(l, -2, -1, seen, d + 1)) return false;
                    lua_pop(l, 2);
                }
            } else if (!equal_value(l, -2, -1, seen, d + 1)) return false;
            lua_pop(l, 2);
        }
        return true;
    }
    int equal(lua_State* l) {
        node(l, 1);
        node(l, 2);
        Pairs seen;
        lua_pushboolean(l, equal_value(l, 1, 2, seen, 0));
        return 1;
    }

    using Hash = lua_Unsigned;
    static Hash mix(Hash h, Hash v) {
        for (std::size_t i = 0; i < sizeof(Hash); ++i) {
            h = (h ^ (v & 255u)) * static_cast<Hash>(1099511628211ull);
            v >>= 8;
        }
        return h;
    }
    static Hash bytes(Hash h, std::string_view s) {
        h = mix(h, s.size());
        for (unsigned char c : s) h = mix(h, c);
        return h;
    }
    Hash hash_value(lua_State* l, int i, std::unordered_map<lua_Integer, Hash>& memo, std::size_t d) {
        Hash h = mix(static_cast<Hash>(14695981039346656037ull), lua_type(l, i));
        if (const auto* n = test(l, i)) {
            depth(d);
            room(l);
            if (auto it = memo.find(n->id); it != memo.end()) return it->second;
            h = bytes(h, n->shape->def->name);
            if (n->shape->ctor) h = bytes(h, n->shape->ctor->name);
            Stack stack(l);
            lua_getiuservalue(l, i, fields_slot);
            const int data = lua_gettop(l);
            for (const auto* f : n->shape->fields) {
                field(l, data, f->name);
                if (sequence(*f)) {
                    const int seq = lua_gettop(l);
                    const auto len = static_cast<lua_Integer>(lua_rawlen(l, seq));
                    h = mix(h, len);
                    for (lua_Integer j = 1; j <= len; ++j) {
                        lua_rawgeti(l, seq, j);
                        h = mix(h, hash_value(l, -1, memo, d + 1));
                        lua_pop(l, 1);
                    }
                } else h = mix(h, hash_value(l, -1, memo, d + 1));
                lua_pop(l, 1);
            }
            memo.emplace(n->id, h);
        } else if (lua_isinteger(l, i)) h = mix(h, static_cast<Hash>(lua_tointeger(l, i)));
        else if (lua_type(l, i) == LUA_TNUMBER) h = mix(h, std::hash<lua_Number>{}(lua_tonumber(l, i)));
        else if (lua_isboolean(l, i)) h = mix(h, lua_toboolean(l, i));
        else if (lua_type(l, i) == LUA_TSTRING) h = bytes(h, text(l, i));
        return h;
    }
    int hash(lua_State* l) {
        node(l, 1);
        std::unordered_map<lua_Integer, Hash> memo;
        lua_pushinteger(l, static_cast<lua_Integer>(hash_value(l, 1, memo, 0)));
        return 1;
    }

    int annotate(lua_State* l) {
        node(l, 1);
        if (lua_isnoneornil(l, 2)) fail("annotation key must not be nil");
        if (lua_gettop(l) < 3) lua_settop(l, 3);
        if (lua_getiuservalue(l, 1, annotations_slot) == LUA_TNIL) {
            lua_pop(l, 1);
            lua_newtable(l);
            lua_pushvalue(l, -1);
            lua_setiuservalue(l, 1, annotations_slot);
        }
        lua_pushvalue(l, 2);
        lua_pushvalue(l, 3);
        lua_rawset(l, -3);
        return 0;
    }
    int annotation(lua_State* l) {
        node(l, 1);
        if (lua_isnone(l, 2)) lua_pushnil(l);
        if (lua_getiuservalue(l, 1, annotations_slot) == LUA_TTABLE) {
            lua_pushvalue(l, 2);
            lua_rawget(l, -2);
        }
        return 1;
    }
    int annotations(lua_State* l) {
        node(l, 1);
        lua_newtable(l);
        const int out = lua_gettop(l);
        if (lua_getiuservalue(l, 1, annotations_slot) == LUA_TTABLE) {
            const int src = lua_gettop(l);
            lua_pushnil(l);
            while (lua_next(l, src)) {
                lua_pushvalue(l, -2);
                lua_pushvalue(l, -2);
                lua_rawset(l, out);
                lua_pop(l, 1);
            }
        }
        lua_settop(l, out);
        return 1;
    }
    int location(lua_State* l) {
        node(l, 1);
        lua_getiuservalue(l, 1, location_slot);
        copy_location(l, -1);
        return 1;
    }
    int provenance(lua_State* l) {
        const auto& n = node(l, 1);
        lua_newtable(l);
        if (n.origin) {
            lua_pushinteger(l, n.origin);
            lua_setfield(l, -2, "origin");
        }
        location(l);
        lua_remove(l, -2);
        lua_setfield(l, -2, "location");
        return 1;
    }

    static void descriptors(lua_State* l, const std::vector<Field>& list, bool attribute = false) {
        lua_newtable(l);
        lua_Integer i = 0;
        for (const auto& f : list) {
            field_descriptor(l, f, attribute);
            lua_rawseti(l, -2, ++i);
        }
    }
    static void descriptor(lua_State* l, const Constructor& c) {
        lua_newtable(l);
        set(l, "name", c.name);
        integer(l, "tag", c.tag);
        descriptors(l, c.fields);
        lua_setfield(l, -2, "fields");
        source_span(l, c.span);
        lua_setfield(l, -2, "span");
    }
    static void constructor_descriptors(lua_State* l, const Definition& def) {
        lua_newtable(l);
        lua_Integer i = 0;
        for (const auto& c : def.constructors) {
            descriptor(l, c);
            lua_rawseti(l, -2, ++i);
        }
    }
    static void descriptor(lua_State* l, const Definition& def) {
        lua_newtable(l);
        set(l, "name", def.name);
        set(l, "kind", def.kind == DefinitionKind::sum ? "sum" : "product");
        lua_pushboolean(l, def.is_enum);
        lua_setfield(l, -2, "enum");
        descriptors(l, def.fields);
        lua_setfield(l, -2, "fields");
        descriptors(l, def.attributes, true);
        lua_setfield(l, -2, "attributes");
        constructor_descriptors(l, def);
        lua_setfield(l, -2, "constructors");
        source_span(l, def.span);
        lua_setfield(l, -2, "span");
    }
    const Definition& definition(lua_State* l, int i) {
        if (const auto* n = test(l, i)) return *n->shape->def;
        const auto name = text(l, i);
        for (const auto& def : schema.definitions) if (def.name == name) return def;
        fail("unknown ASDL type: " + std::string(name));
    }
    int describe(lua_State* l) {
        const auto& def = definition(l, 1);
        if (!lua_isnoneornil(l, 2)) {
            const auto& s = shape(def.name, text(l, 2));
            descriptor(l, *s.ctor);
        } else descriptor(l, def);
        return 1;
    }
    int constructors(lua_State* l) { constructor_descriptors(l, definition(l, 1)); return 1; }
    int reflect(lua_State* l) {
        lua_newtable(l);
        set(l, "file", schema.file);
        set(l, "module", schema.module);
        set(l, "language", schema.language);
        set(l, "summary", schema.summary);
        set(l, "version", schema.version);
        set(l, "unit", schema.unit);
        lua_newtable(l);
        lua_Integer i = 0;
        for (const auto& def : schema.definitions) {
            descriptor(l, def);
            lua_rawseti(l, -2, ++i);
        }
        lua_setfield(l, -2, "definitions");
        return 1;
    }

    int option(lua_State* l) {
        const auto key = text(l, 1);
        if (auto it = options.values.find(std::string(key)); it != options.values.end()) push(l, it->second);
        else if (auto it = schema.options.find(std::string(key)); it != schema.options.end()) push(l, it->second);
        else lua_pushnil(l);
        return 1;
    }
    std::size_t count(std::string_view severity) const {
        if (!severity.empty()) {
            const auto it = diagnostics.find(severity);
            return it == diagnostics.end() ? 0 : it->second;
        }
        std::size_t total = 0;
        for (const auto& [k, v] : diagnostics) total += v;
        return total;
    }
    int diagnostic_count(lua_State* l) {
        lua_pushinteger(l, static_cast<lua_Integer>(count(lua_isnoneornil(l, 1)
            ? std::string_view{} : text(l, 1))));
        return 1;
    }
    static inline char diagnostic_records;
    int emit(lua_State* l) {
        if (!lua_istable(l, 1)) fail("expected diagnostic record");
        field(l, 1, "severity");
        const auto sev = text(l, -1);
        if (sev != "error" && sev != "warning" && sev != "info" &&
            sev != "note" && sev != "fatal") fail("invalid diagnostic severity");
        lua_pop(l, 1);
        lua_newtable(l);
        const int record = lua_gettop(l);
        set(l, "severity", sev);
        field(l, 1, "message");
        text(l, -1); // Require a string before moving it into the record.
        put(l, record, "message");
        if (field(l, 1, "code") != LUA_TNIL) {
            if (lua_type(l, -1) != LUA_TSTRING && !lua_isinteger(l, -1))
                fail("diagnostic code must be a string or integer");
            lua_setfield(l, record, "code");
        } else lua_pop(l, 1);
        field(l, 1, "location");
        copy_location(l, -1);
        lua_setfield(l, record, "location");
        lua_pop(l, 1);
        lua_rawgetp(l, LUA_REGISTRYINDEX, &diagnostic_records);
        lua_pushvalue(l, record);
        lua_rawseti(l, -2, static_cast<lua_Integer>(lua_rawlen(l, -2) + 1));
        ++diagnostics[std::string(sev)];
        return 0;
    }
    int records(lua_State* l) {
        lua_rawgetp(l, LUA_REGISTRYINDEX, &diagnostic_records);
        const int data = lua_gettop(l);
        lua_newtable(l);
        const int out = lua_gettop(l);
        const auto len = static_cast<lua_Integer>(lua_rawlen(l, data));
        for (lua_Integer i = 1; i <= len; ++i) {
            lua_rawgeti(l, data, i);
            copy_location(l, -1);
            lua_rawseti(l, out, i);
            lua_pop(l, 1);
        }
        return 1;
    }

    int stringify(lua_State* l) {
        const auto& n = node(l, 1);
        lua_pushfstring(l, "asdl.%s%s%s#%I", n.shape->def->name.c_str(), n.shape->ctor ? "." : "",
            n.shape->ctor ? n.shape->ctor->name.c_str() : "", n.id);
        return 1;
    }

    int install(lua_State* l) {
        struct Library { const char* name; lua_CFunction open; };
        constexpr Library safe[] = {{"_G", luaopen_base}, {LUA_TABLIBNAME, luaopen_table},
            {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
        for (const auto& lib : safe) {
            luaL_requiref(l, lib.name, lib.open, 1);
            lua_pop(l, 1);
        }
        // No package, coroutine, debug, process or filesystem access.
        for (const char* name : {"dofile", "loadfile", "load"}) {
            lua_pushnil(l);
            lua_setglobal(l, name);
        }
        static const luaL_Reg mt[] = {{"__tostring", invoke<&Impl::stringify>}, {nullptr, nullptr}};
        luaL_newmetatable(l, node_mt);
        luaL_setfuncs(l, mt, 0);
        set(l, "__metatable", "ASDL node");
        lua_pop(l, 1);
        static const luaL_Reg api[] = {
            {"isnode", invoke<&Impl::isnode>}, {"type", invoke<&Impl::type>}, {"tag", invoke<&Impl::tag>},
            {"fields", invoke<&Impl::fields>}, {"destruct", invoke<&Impl::destruct>},
            {"children", invoke<&Impl::children>}, {"get", invoke<&Impl::get>}, {"has", invoke<&Impl::has>},
            {"schema", invoke<&Impl::reflect>}, {"describe", invoke<&Impl::describe>},
            {"constructors", invoke<&Impl::constructors>},
            {"make", invoke<&Impl::make>}, {"construct", invoke<&Impl::construct>},
            {"set", invoke<&Impl::assign>}, {"replace", invoke<&Impl::replace>},
            {"copy", invoke<&Impl::copy>}, {"clone", invoke<&Impl::clone>},
            {"map_children", invoke<&Impl::map_children>}, {"equal", invoke<&Impl::equal>},
            {"hash", invoke<&Impl::hash>}, {"validate", invoke<&Impl::validate>},
            {"freeze", invoke<&Impl::freeze>}, {"is_frozen", invoke<&Impl::frozen>},
            {"annotate", invoke<&Impl::annotate>}, {"annotation", invoke<&Impl::annotation>},
            {"annotations", invoke<&Impl::annotations>},
            {"id", invoke<&Impl::id>}, {"same", invoke<&Impl::same>}, {"kind", invoke<&Impl::kind>},
            {"is_sum", invoke<&Impl::is_sum>}, {"is_product", invoke<&Impl::is_product>},
            {"is_enum", invoke<&Impl::is_enum>},
            {"location", invoke<&Impl::location>}, {"provenance", invoke<&Impl::provenance>},
            {"option", invoke<&Impl::option>}, {nullptr, nullptr}
        };
        static const luaL_Reg diagnostic[] = {{"emit", invoke<&Impl::emit>},
            {"count", invoke<&Impl::diagnostic_count>}, {"records", invoke<&Impl::records>}, {nullptr, nullptr}};
        lua_newtable(l);
        lua_rawsetp(l, LUA_REGISTRYINDEX, &diagnostic_records);
        luaL_newlib(l, api);
        luaL_newlib(l, diagnostic);
        lua_setfield(l, -2, "diagnostic");
        lua_setglobal(l, "asdl");
        lua_newtable(l);
        lua_Integer index = 0;
        for (const auto& arg : options.arguments) {
            push(l, arg);
            lua_rawseti(l, -2, ++index);
        }
        lua_setglobal(l, "arg");
        return 0;
    }
};

LuaSession::LuaSession(const Schema& schema, LuaOptions options)
    : impl_(std::make_unique<Impl>(schema, std::move(options))) {
#if LUA_VERSION_NUM >= 505
    impl_->lua = lua_newstate(Impl::alloc, impl_.get(), luaL_makeseed(nullptr));
#else
    impl_->lua = lua_newstate(Impl::alloc, impl_.get());
#endif
    if (!impl_->lua) fail("unable to allocate ASDL Lua state");
    *static_cast<Impl**>(lua_getextraspace(impl_->lua)) = impl_.get();
    lua_pushcfunction(impl_->lua, Impl::invoke<&Impl::install>);
    require_ok(impl_->lua, lua_pcall(impl_->lua, 0, 0, 0));
}

LuaSession::~LuaSession() = default;
lua_State* LuaSession::state() const noexcept { return impl_->lua; }

void LuaSession::check(std::string_view source, std::string_view chunk) {
    auto* l = state();
    Stack stack(l);
    room(l);
    const std::string name(chunk);
    require_ok(l, luaL_loadbufferx(l, source.data() ? source.data() : "", source.size(), name.c_str(), "t"));
}

void LuaSession::run(std::string_view source, std::string_view chunk) {
    auto* l = state();
    Stack stack(l);
    room(l);
    const std::string name(chunk);
    lua_pushcfunction(l, traceback);
    const int handler = lua_gettop(l);
    require_ok(l, luaL_loadbufferx(l, source.data() ? source.data() : "", source.size(), name.c_str(), "t"));
    Impl::Execution budget(*impl_);
    const int status = lua_pcall(l, 0, 0, handler);
    if (impl_->exhausted && status == LUA_OK) fail("ASDL Lua instruction limit exceeded");
    require_ok(l, status);
}

void LuaSession::extend(int (*install)(lua_State*), void* context) {
    if (!install) fail("missing ASDL Lua installer");
    auto* l = state();
    Stack stack(l);
    room(l);
    lua_pushcfunction(l, traceback);
    const int handler = lua_gettop(l);
    lua_pushcfunction(l, install);
    lua_pushglobaltable(l);
    field(l, -1, "asdl");
    lua_remove(l, -2);
    if (!lua_istable(l, -1)) fail("ASDL Lua API table not available");
    lua_pushlightuserdata(l, context);
    Impl::Execution budget(*impl_);
    const int status = lua_pcall(l, 2, 0, handler);
    if (impl_->exhausted && status == LUA_OK) fail("ASDL Lua instruction limit exceeded");
    require_ok(l, status);
}

std::size_t LuaSession::diagnostic_count(std::string_view severity) const {
    return impl_->count(severity);
}

} // namespace asdl
