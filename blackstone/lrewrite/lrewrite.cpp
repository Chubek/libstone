#include "lrewrite.hpp"
#include "../lua/runtime.hpp"
#include <libir/LoopAnalysis.hpp>
#include <array>

namespace blackstone::lua {
using namespace firestone::ir;
static constexpr std::array<const char*, 28> op_names = {
    "constant", "copy", "add", "sub", "mul", "div", "mod", "and", "or", "xor",
    "shl", "shr", "neg", "not", "eq", "ne", "lt", "le", "gt", "ge", "phi",
    "select", "load", "store", "call", "jump", "branch", "return"
};
static const char* kind_name(TypeKind kind) {
    switch (kind) {
    case TypeKind::Integer: return "integer";
    case TypeKind::Float: return "float";
    case TypeKind::Pointer: return "pointer";
    case TypeKind::Memory: return "memory";
    case TypeKind::Function: return "function";
    case TypeKind::Void: return "void";
    default: return "unknown";
    }
}
static void text(lua_State* L, const char* key, const std::string& value) {
    lua_pushlstring(L, value.data(), value.size()); lua_setfield(L, -2, key);
}
static void number(lua_State* L, const char* key, lua_Integer value) {
    lua_pushinteger(L, value); lua_setfield(L, -2, key);
}
static lua_Integer number_field(lua_State* L, int table, const char* key) {
    raw_field(L, table, key);
    if (!lua_isinteger(L, -1)) throw std::runtime_error(std::string("expected integer field: ") + key);
    auto result = lua_tointeger(L, -1); lua_pop(L, 1); return result;
}
static void push_ids(lua_State* L, const std::vector<BlockId>& ids) {
    lua_newtable(L);
    for (size_t n = 0; n < ids.size(); ++n) { lua_pushinteger(L, ids[n]); lua_rawseti(L, -2, n + 1); }
}
static std::vector<BlockId> read_ids(lua_State* L, int table, const char* key) {
    raw_field(L, table, key);
    if (!lua_istable(L, -1)) throw std::runtime_error(std::string("expected list: ") + key);
    std::vector<BlockId> result;
    for (size_t n = 1; n <= lua_rawlen(L, -1); ++n) {
        lua_rawgeti(L, -1, n);
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) throw std::runtime_error("invalid block ID");
        result.push_back(lua_tointeger(L, -1)); lua_pop(L, 1);
    }
    lua_pop(L, 1); return result;
}
static void push_type(lua_State* L, const Type& type) {
    text(L, "kind", kind_name(type.kind)); number(L, "bits", type.bits);
    text(L, "type_name", type.name);
}
static Type read_type(lua_State* L, int table, std::string kind = {}) {
    Type result;
    if (kind.empty()) kind = string_field(L, table, "kind");
    for (auto k : {TypeKind::Unknown, TypeKind::Void, TypeKind::Integer, TypeKind::Float,
                   TypeKind::Pointer, TypeKind::Memory, TypeKind::Function})
        if (kind == kind_name(k)) result.kind = k;
    if (result.kind == TypeKind::Unknown && kind != "unknown") throw std::runtime_error("invalid type kind");
    auto bits = number_field(L, table, "bits");
    if (bits < 0 || bits > 65536) throw std::runtime_error("invalid type width");
    result.bits = static_cast<unsigned>(bits);
    raw_field(L, table, "type_name");
    if (lua_type(L, -1) == LUA_TSTRING) result.name = lua_tostring(L, -1);
    lua_pop(L, 1); return result;
}
static void push_operand(lua_State* L, const Operand& operand) {
    lua_newtable(L); push_type(L, operand.type);
    switch (operand.kind) {
    case OperandKind::Value: text(L, "kind", "value"); number(L, "value", operand.value); break;
    case OperandKind::Integer: number(L, "integer", operand.integer); break;
    case OperandKind::Float: lua_pushnumber(L, operand.floating); lua_setfield(L, -2, "floating"); break;
    case OperandKind::Symbol: text(L, "kind", "symbol"); text(L, "symbol", operand.symbol); break;
    }
    // Operand kind and type kind can differ for symbols; retain both.
    text(L, "type_kind", kind_name(operand.type.kind));
}
static Operand read_operand(lua_State* L, int table) {
    auto kind = string_field(L, table, "kind");
    if (kind == "value") {
        auto id = number_field(L, table, "value");
        if (id < 0) throw std::runtime_error("invalid value ID");
        return Operand::ref(id);
    }
    if (kind == "integer") return Operand::constant(number_field(L, table, "integer"), read_type(L, table));
    if (kind == "float") {
        raw_field(L, table, "floating");
        if (lua_type(L, -1) != LUA_TNUMBER) throw std::runtime_error("invalid floating constant");
        auto n = lua_tonumber(L, -1); lua_pop(L, 1); return Operand::real(n, read_type(L, table));
    }
    if (kind == "symbol") {
        auto name = string_field(L, table, "symbol");
        auto type = read_type(L, table, string_field(L, table, "type_kind"));
        return Operand::named(name, type);
    }
    throw std::runtime_error("invalid operand kind");
}
static void push_instruction(lua_State* L, const Instruction& inst) {
    lua_newtable(L);
    text(L, "op", op_names.at(static_cast<size_t>(inst.opcode)));
    if (inst.result) number(L, "result_id", *inst.result);
    push_type(L, inst.type); text(L, "annotation", inst.annotation);
    lua_newtable(L);
    for (size_t n = 0; n < inst.operands.size(); ++n) { push_operand(L, inst.operands[n]); lua_rawseti(L, -2, n + 1); }
    lua_setfield(L, -2, "operands");
    push_ids(L, inst.targets); lua_setfield(L, -2, "targets");
    push_ids(L, inst.incoming_blocks); lua_setfield(L, -2, "incoming");
}
static Instruction read_instruction(lua_State* L, int table) {
    Instruction inst;
    auto op = string_field(L, table, "op");
    auto found = std::find(op_names.begin(), op_names.end(), op);
    if (found == op_names.end()) throw std::runtime_error("unsupported SSA opcode: " + op);
    inst.opcode = static_cast<Opcode>(found - op_names.begin()); inst.type = read_type(L, table);
    raw_field(L, table, "result_id");
    if (!lua_isnil(L, -1)) {
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) throw std::runtime_error("invalid result ID");
        inst.result = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    raw_field(L, table, "annotation");
    if (lua_type(L, -1) == LUA_TSTRING) inst.annotation = lua_tostring(L, -1);
    lua_pop(L, 1);
    inst.targets = read_ids(L, table, "targets"); inst.incoming_blocks = read_ids(L, table, "incoming");
    raw_field(L, table, "operands");
    if (!lua_istable(L, -1)) throw std::runtime_error("expected operand list");
    auto count = lua_rawlen(L, -1);
    for (size_t n = 1; n <= count; ++n) {
        lua_rawgeti(L, -1, n);
        if (!lua_istable(L, -1)) throw std::runtime_error("expected operand table");
        inst.operands.push_back(read_operand(L, lua_absindex(L, -1))); lua_pop(L, 1);
    }
    lua_pop(L, 1); return inst;
}
void push_ssa_context(lua_State* L, const void* raw, bool modular) {
    const auto& graph = *static_cast<const SimpleSSAGraph*>(raw);
    lua_newtable(L); int context = lua_absindex(L, -1);
    number(L, "next_value", graph.value_count());
    text(L, "integer_semantics", modular ? "modular" : "unknown");
    lua_newtable(L); lua_setfield(L, context, "new_types");
    lua_newtable(L);
    for (ValueId id = 0; id < graph.value_count(); ++id) {
        lua_newtable(L); push_type(L, graph.type(id)); lua_rawseti(L, -2, id);
    }
    lua_setfield(L, context, "value_types");
    lua_newtable(L);
    for (auto b : graph.cfg().blocks()) {
        lua_newtable(L); size_t n = 0;
        for (const auto& inst : graph.cfg().block(b).instructions) { push_instruction(L, inst); lua_rawseti(L, -2, ++n); }
        lua_rawseti(L, -2, b);
    }
    lua_setfield(L, context, "blocks");
    lua_newtable(L);
    for (const auto& item : graph.cfg().dominators()) {
        lua_newtable(L);
        for (auto b : item.second) { lua_pushboolean(L, true); lua_rawseti(L, -2, b); }
        lua_rawseti(L, -2, item.first);
    }
    lua_setfield(L, context, "dominators");
    lua_newtable(L); size_t n = 0;
    for (const auto& loop : natural_loops(graph.cfg())) {
        lua_newtable(L); number(L, "header", loop.header);
        push_ids(L, {loop.blocks.begin(), loop.blocks.end()}); lua_setfield(L, -2, "blocks");
        lua_newtable(L);
        for (auto b : loop.blocks) { lua_pushboolean(L, true); lua_rawseti(L, -2, b); }
        lua_setfield(L, -2, "members");
        lua_newtable(L);
        for (auto b : loop.latches) { lua_pushboolean(L, true); lua_rawseti(L, -2, b); }
        lua_setfield(L, -2, "latches"); lua_rawseti(L, -2, ++n);
    }
    lua_setfield(L, context, "loop_list");
    lua_getglobal(L, "lrewrite"); lua_getfield(L, -1, "ssa_context"); lua_remove(L, -2);
    lua_pushvalue(L, context); checked_call(L, 1, 1); lua_remove(L, context);
}
void read_ssa_context(lua_State* L, int context, void* raw) {
    auto& graph = *static_cast<SimpleSSAGraph*>(raw);
    context = lua_absindex(L, context);
    raw_field(L, context, "new_types");
    if (!lua_istable(L, -1)) throw std::runtime_error("expected new_types list");
    auto count = lua_rawlen(L, -1);
    for (size_t n = 1; n <= count; ++n) {
        lua_rawgeti(L, -1, n);
        if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 2 || lua_tointeger(L, -1) > 64)
            throw std::runtime_error("unsupported new integer type");
        graph.reserve_value(Type::integer(lua_tointeger(L, -1))); lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (number_field(L, context, "next_value") != static_cast<lua_Integer>(graph.value_count()))
        throw std::runtime_error("new value declarations do not match allocated values");
    raw_field(L, context, "blocks");
    if (!lua_istable(L, -1)) throw std::runtime_error("expected blocks table");
    int blocks = lua_absindex(L, -1);
    for (auto b : graph.cfg().blocks()) {
        lua_rawgeti(L, blocks, b);
        if (!lua_istable(L, -1)) throw std::runtime_error("kernel removed a CFG block");
        auto size = lua_rawlen(L, -1); std::vector<Instruction> instructions;
        for (size_t n = 1; n <= size; ++n) {
            lua_rawgeti(L, -1, n);
            if (!lua_istable(L, -1)) throw std::runtime_error("expected instruction table");
            instructions.push_back(read_instruction(L, lua_absindex(L, -1))); lua_pop(L, 1);
        }
        auto& block = const_cast<BasicBlock&>(graph.cfg().block(b));
        if (instructions.empty() || block.instructions.back().opcode != instructions.back().opcode ||
            block.instructions.back().targets != instructions.back().targets)
            throw std::runtime_error("SSA adapter does not support CFG changes");
        block.instructions = std::move(instructions); lua_pop(L, 1);
    }
    lua_pop(L, 1);
}
} // namespace blackstone::lua
