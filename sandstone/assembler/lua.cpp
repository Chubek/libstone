/* Bundled Lua + Kaguya. Each assembler owns its state and output buffer. */
#include "intern.h"
#include "KaguyaCompat.hpp"
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
struct Runtime {
    kaguya::State lua;
    std::string output, error;
    std::map<std::string, kaguya::LuaFunction> directives;
    explicit Runtime(sandstone_asm_t *ctx) {
        lua.setErrorHandler([this](int, const char *message) { error = message ? message : "Lua error"; });
        lua["lsandas"] = kaguya::NewTable();
        auto api = lua["lsandas"].get<kaguya::LuaTable>();
        api["_emit"] = kaguya::function([this](const std::string &text) {
            if (text.size() > 16 * 1024 * 1024 - output.size())
                throw std::runtime_error("Lua output exceeds 16 MiB");
            if (text.find('\0') != std::string::npos) throw std::runtime_error("NUL in generated assembly");
            output += text;
        });
        api["register_directive"] = kaguya::function([this](const std::string &name, kaguya::LuaFunction fn) {
            if (sand_extension_reserved(name.c_str()) || fn.isNilref() || directives.count(name))
                throw std::runtime_error("invalid, reserved or duplicate Lua directive: " + name);
            directives.emplace(name, std::move(fn));
        });
        auto symas = lua.newTable();
        auto targets = lua.newTable();
        for (uint32_t i = 0; i < domsymas_target_count(ctx->bank); ++i) {
            const auto *t = domsymas_target_at(ctx->bank, i);
            auto target = lua.newTable();
            target["arch"] = t->arch; target["subarch"] = t->subarch;
            target["addr_size"] = t->addr_size; target["default_op_size"] = t->default_op_size;
            target["default_addr_size"] = t->default_addr_size; target["obj_format"] = t->obj_format;
            auto opcodes = lua.newTable();
            for (uint32_t j = 0; j < domsymas_mnemonic_count(ctx->bank); ++j) {
                const auto *m = domsymas_mnemonic_at(ctx->bank, j);
                auto opcode = lua.newTable(); opcode["name"] = m->canonical; opcode["id"] = m->id;
                auto forms = lua.newTable();
                for (uint32_t k = 0; k < m->form_count; ++k) {
                    const auto *f = domsymas_find_form(ctx->bank, m->forms[k]);
                    if (!f) continue;
                    auto form = lua.newTable(); form["id"] = f->id; form["operand_count"] = f->operand_count;
                    auto operands = lua.newTable();
                    for (uint32_t n = 0; n < domsymas_operand_count(ctx->bank, f->id); ++n) {
                        const auto *o = domsymas_operand_at(ctx->bank, f->id, n);
                        auto operand = lua.newTable(); operand["position"] = o->position;
                        operand["kind"] = o->kind; operand["width"] = o->width; operand["constraint"] = o->constraint;
                        operands[n + 1] = operand;
                    }
                    form["operands"] = operands; forms[k + 1] = form;
                }
                opcode["forms"] = forms; opcodes[m->canonical] = opcode;
            }
            target["opcodes"] = opcodes; targets[t->arch] = target;
        }
        symas["targets"] = targets;
        auto regs = lua.newTable(), features = lua.newTable(), sections = lua.newTable();
        auto relocs = lua.newTable(), syntaxes = lua.newTable(), linkages = lua.newTable();
        for (uint32_t i = 0; i < domsymas_register_count(ctx->bank); ++i) {
            const auto *r = domsymas_register_at(ctx->bank, i); auto v = lua.newTable();
            v["id"] = r->id; v["class"] = r->regclass; v["width"] = r->width;
            v["encoding"] = r->enc_num; v["parent"] = r->parent; regs[r->name] = v;
        }
        for (uint32_t i = 0; i < domsymas_feature_count(ctx->bank); ++i) {
            const auto *r = domsymas_feature_at(ctx->bank, i); auto v = lua.newTable();
            v["id"] = r->id; v["flags"] = r->flags; v["description"] = r->desc; features[r->name] = v;
        }
        for (uint32_t i = 0; i < domsymas_section_count(ctx->bank); ++i) {
            const auto *r = domsymas_section_at(ctx->bank, i); auto v = lua.newTable();
            v["id"] = r->id; v["flags"] = r->flags; v["alignment"] = r->align;
            v["address_space"] = r->addrspace; v["content"] = r->content; sections[r->name] = v;
        }
        for (uint32_t i = 0; i < domsymas_reloc_count(ctx->bank); ++i) {
            const auto *r = domsymas_reloc_at(ctx->bank, i); auto v = lua.newTable();
            v["id"] = r->id; v["width"] = r->width; v["offset"] = r->offset;
            v["signed"] = r->is_signed; v["pc_relative"] = r->pcrel; v["expression"] = r->expr; relocs[r->name] = v;
        }
        for (uint32_t i = 0; i < domsymas_syntax_count(ctx->bank); ++i) {
            const auto *r = domsymas_syntax_at(ctx->bank, i); auto v = lua.newTable();
            v["id"] = r->id; v["operand_order"] = r->operand_order; v["register_prefix"] = r->reg_prefix;
            v["immediate_prefix"] = r->imm_prefix; v["comment"] = r->comment; v["separator"] = r->separator; syntaxes[r->name] = v;
        }
        for (uint32_t i = 0; i < domsymas_linkage_count(ctx->bank); ++i) {
            const auto *r = domsymas_linkage_at(ctx->bank, i); auto v = lua.newTable();
            v["id"] = r->id; v["binding"] = r->binding; v["visibility"] = r->visibility;
            v["symbol_type"] = r->symtype; v["flags"] = r->flags; linkages[r->name] = v;
        }
        symas["registers"] = regs; symas["features"] = features; symas["sections"] = sections;
        symas["relocations"] = relocs; symas["syntaxes"] = syntaxes; symas["linkages"] = linkages;
        api["symas"] = symas;
        run(R"lua(
function lsandas.inject(value)
  if type(value) == 'table' then
    local items = {}
    for i, v in ipairs(value) do
      assert(type(v) == 'string' or type(v) == 'number', 'inject expects strings/numbers')
      items[i] = tostring(v)
    end
    lsandas._emit(table.concat(items, ', '))
  else
    assert(type(value) == 'string' or type(value) == 'number', 'inject expects string/number/table')
    lsandas._emit(tostring(value))
  end
end
function lsandas.injectf(fmt, ...)
  assert(type(fmt) == 'string', 'injectf expects format string')
  local args = {...}
  if #args == 1 and type(args[1]) == 'table' then args = args[1] end
  local normalized = fmt:gsub('~li', '%%d'):gsub('~lx', '%%x'):gsub('~s', '%%s')
  local slots = 0
  normalized:gsub('%%(.)', function(c) if c ~= '%%' then slots = slots + 1 end end)
  assert(slots == 0 and #args == 0 or slots > 0 and #args > 0 and #args % slots == 0,
         'injectf argument count mismatch')
  if slots == 0 then lsandas._emit(string.format(normalized)); return end
  for first = 1, #args, slots do
    local text = string.format(normalized, table.unpack(args, first, first + slots - 1))
    if text:sub(-1) ~= '\n' then text = text .. '\n' end
    lsandas._emit(text)
  end
end
)lua");
    }
    void run(const std::string &source) {
        error.clear();
        if (!lua.dostring(source) || !error.empty()) throw std::runtime_error(error.empty() ? "Lua failed" : error);
    }
};
Runtime &runtime(sandstone_asm_t *ctx) {
    if (!ctx->lua_state) ctx->lua_state = new Runtime(ctx);
    return *static_cast<Runtime *>(ctx->lua_state);
}
}
void sand_lua_reset(sandstone_asm_t *ctx) {
    if (ctx) { delete static_cast<Runtime *>(ctx->lua_state); ctx->lua_state = nullptr; }
}
void sand_lua_constant(sandstone_asm_t *ctx, const char *name, int64_t value) {
    try { runtime(ctx).lua[name] = value; }
    catch (const std::exception &e) { sand_set_error(ctx, "Lua: %s", e.what()); }
}
char *sand_run_lua_block(sandstone_asm_t *ctx, const char *code) {
    try {
        auto &r = runtime(ctx); r.output.clear(); r.run(code);
        return sand_strdup(r.output.c_str());
    } catch (const std::exception &e) { sand_set_error(ctx, "Lua block: %s", e.what()); return nullptr; }
}
int sand_lua_expand(sandstone_asm_t *ctx, const char *name, const char *args, char **out) {
    if (!ctx->lua_state) return 0;
    try {
        auto &r = runtime(ctx); auto it = r.directives.find(name);
        if (it == r.directives.end()) return 0;
        r.output.clear(); r.error.clear();
        it->second.call<void>(std::string(args));
        if (!r.error.empty()) throw std::runtime_error(r.error);
        *out = sand_strdup(r.output.c_str());
        if (!*out) throw std::bad_alloc();
        return 1;
    } catch (const std::exception &e) { sand_set_error(ctx, "%s: %s", name, e.what()); return -1; }
}
int sandstone_asm_load_hook(sandstone_asm_t *ctx, const char *path) {
    if (!ctx) return -1;
    try {
        if (path && *path) {
            Runtime check(ctx);
            auto fn = check.lua.loadfile(path); // Validate syntax without running user code twice.
            if (fn.isNilref() || !check.error.empty()) throw std::runtime_error(check.error);
        }
        char *next = path && *path ? sand_strdup(path) : nullptr;
        if (path && *path && !next) throw std::bad_alloc();
        free(ctx->hook_path); ctx->hook_path = next;
        return 0;
    } catch (const std::exception &e) { sand_set_error(ctx, "Lua hook: %s", e.what()); return -1; }
}
char *sand_run_hook(sandstone_asm_t *ctx, const char *source, const char *name) {
    if (!ctx->hook_path) return sand_strdup(source);
    try {
        auto &r = runtime(ctx); r.error.clear();
        if (!r.lua.dofile(ctx->hook_path) || !r.error.empty()) throw std::runtime_error(r.error);
        kaguya::LuaFunction fn = r.lua["preprocess"];
        if (fn.isNilref()) throw std::runtime_error("hook must define preprocess(source, name)");
        auto result = fn.call<kaguya::LuaRef>(std::string(source), std::string(name));
        if (!r.error.empty()) throw std::runtime_error(r.error);
        if (result.type() != LUA_TSTRING) throw std::runtime_error("preprocess must return a string");
        auto text = result.get<std::string>();
        if (text.find('\0') != std::string::npos) throw std::runtime_error("NUL in preprocessed source");
        return sand_strdup(text.c_str());
    } catch (const std::exception &e) { sand_set_error(ctx, "hook '%s': %s", ctx->hook_path, e.what()); return nullptr; }
}
char *sandstone_asm_preprocess(sandstone_asm_t *ctx, const char *source) {
    return ctx && source ? sand_run_hook(ctx, source, "<preprocess>") : nullptr;
}
