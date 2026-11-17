#include "intern.h"
#include "sdk.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#ifdef SANDSTONE_ASM_ARCHML
#include "ArchML.hpp"
#endif

namespace {
constexpr size_t limit = 16 * 1024 * 1024;
struct Handler { sandstone_source_handler call; void *user; };
struct Extensions {
    std::map<std::string, Handler> handlers;
    std::map<std::string, std::string> macros;
#ifdef SANDSTONE_ASM_ARCHML
    std::unique_ptr<sandstone::archml::Document> arch;
#endif
};
Extensions &extensions(sandstone_asm_t *ctx) {
    if (!ctx->extensions) ctx->extensions = new Extensions;
    return *static_cast<Extensions *>(ctx->extensions);
}
struct Output { std::string text; bool failed = false; };
int sink(void *p, const char *text, size_t size) {
    auto &out = *static_cast<Output *>(p);
    if (out.failed || (!text && size) || size > limit - out.text.size() || (size && std::memchr(text, 0, size))) {
        out.failed = true; return -1;
    }
    try { if (size) out.text.append(text, size); return 0; }
    catch (...) { out.failed = true; return -1; }
}
}
int sand_extension_reserved(const char *name) {
    const std::string names = " .text .data .rodata .bss .section .global .globl .extern .local"
        " .equ .set .type .size .byte .short .hword .2byte .word .long .4byte .quad .8byte"
        " .ascii .asciz .string .zero .space .skip .fill .align .balign .p2align .org"
        " .macro .endmacro .endm .purge .if .ifdef .ifndef .elseif .else .endif .rept .endr"
        " .struct .field .endstruct .enum .endenum .include .incbin .error .assert .insn .uleb128 .sleb128"
        " .pushsection .popsection .previous ";
    return !name || !sand_is_name(name) || names.find(" " + std::string(name) + " ") != std::string::npos;
}
int sandstone_asm_register_directive(sandstone_asm_t *ctx, const char *name,
    sandstone_source_handler fn, void *user) {
    if (!ctx) return -1;
    try {
        if (!fn || sand_extension_reserved(name)) throw std::runtime_error("invalid or reserved extension name");
        auto &e = extensions(ctx);
        if (e.macros.count(name) || !e.handlers.emplace(name, Handler{fn, user}).second)
            throw std::runtime_error("duplicate extension name");
        return 0;
    } catch (const std::exception &e) { sand_set_error(ctx, "%s", e.what()); return -1; }
}
int sandstone_asm_unregister_directive(sandstone_asm_t *ctx, const char *name) {
    if (!ctx || !name || !ctx->extensions) return -1;
    return static_cast<Extensions *>(ctx->extensions)->handlers.erase(name) ? 0 : -1;
}
int sandstone_asm_define_macro(sandstone_asm_t *ctx, const char *name,
    const char *params, const char *body) {
    if (!ctx) return -1;
    try {
        if (sand_extension_reserved(name) || !params || !body || std::strchr(params, '\n') || std::strchr(params, '\r'))
            throw std::runtime_error("invalid macro definition");
        auto &e = extensions(ctx);
        if (e.handlers.count(name) || e.macros.count(name)) throw std::runtime_error("duplicate macro name");
        e.macros[name] = ".macro " + std::string(name) + " " + params + "\n" + body + "\n.endmacro\n";
        return 0;
    } catch (const std::exception &e) { sand_set_error(ctx, "%s", e.what()); return -1; }
}
char *sand_extension_prelude(sandstone_asm_t *ctx) {
    try {
        Output out;
        if (ctx->extensions) for (const auto &[name, body] : extensions(ctx).macros) {
            (void)name;
            if (sink(&out, body.data(), body.size())) throw std::runtime_error("macro prelude exceeds 16 MiB");
        }
        return sand_strdup(out.text.c_str());
    } catch (const std::exception &e) { sand_set_error(ctx, "%s", e.what()); return nullptr; }
}
int sand_extension_expand(sandstone_asm_t *ctx, const char *name, const char *args, char **result) {
    *result = nullptr;
    try {
        std::string out;
        if (std::strcmp(name, ".insn") == 0) {
#ifdef SANDSTONE_ASM_ARCHML
            auto &e = extensions(ctx);
            if (!e.arch) throw std::runtime_error(".insn requires an ArchML profile");
            std::istringstream in(args);
            std::string op, arg;
            std::getline(in, op, ',');
            op = sand_trim(op.data());
            std::vector<uint64_t> values;
            while (std::getline(in, arg, ',')) {
                int ok = 0;
                auto value = sand_eval_expr(ctx, arg.c_str(), &ok);
                if (!ok) throw std::runtime_error(".insn requires constant operands: " + arg);
                values.push_back(static_cast<uint64_t>(value));
            }
            const auto bytes = e.arch->encode(op, values);
            out = ".byte ";
            for (size_t i = 0; i < bytes.size(); ++i) out += (i ? "," : "") + std::to_string(bytes[i]);
            out += '\n';
#else
            throw std::runtime_error("ArchML support was disabled at build time");
#endif
        } else {
            if (!ctx->extensions) return sand_lua_expand(ctx, name, args, result);
            auto &e = extensions(ctx);
            auto it = e.handlers.find(name);
            if (it == e.handlers.end()) return sand_lua_expand(ctx, name, args, result);
            char error[1024] = {};
            Output generated;
            auto handler = it->second;
            int status = handler.call(handler.user, args, sink, &generated, error, sizeof error);
            error[sizeof error - 1] = 0;
            if (generated.failed) throw std::runtime_error("invalid or oversized generated source");
            if (status)
                throw std::runtime_error(error[0] ? error : "extension failed");
            out = std::move(generated.text);
        }
        *result = sand_strdup(out.c_str());
        if (!*result) throw std::bad_alloc();
        return 1;
    } catch (const std::exception &e) { sand_set_error(ctx, "%s: %s", name, e.what()); return -1; }
    catch (...) { sand_set_error(ctx, "%s: extension threw an exception", name); return -1; }
}
void sand_extension_free(sandstone_asm_t *ctx) {
    delete static_cast<Extensions *>(ctx->extensions);
    ctx->extensions = nullptr;
}
int sandstone_asm_load_archml(sandstone_asm_t *ctx, const char *path) {
    if (!ctx || !path) return -1;
    try {
#ifdef SANDSTONE_ASM_ARCHML
        auto doc = std::make_unique<sandstone::archml::Document>(sandstone::archml::Document::load(path));
        const auto *target = doc->find("target", doc->name());
        if (target) if (const auto *endian = target->property("endian"))
            if (endian->children.size() == 2) ctx->byte_order = endian->children[1].text == "big";
        extensions(ctx).arch = std::move(doc);
        return 0;
#else
        throw std::runtime_error("ArchML support was disabled at build time");
#endif
    } catch (const std::exception &e) { sand_set_error(ctx, "%s", e.what()); return -1; }
}
