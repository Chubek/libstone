// Serialize the section-relative results of the native SymAS encoder. All
// integer fields are explicitly little-endian, independent of host layout.
#include "intern.h"
#include "object_reloc.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
void put(Bytes &b, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) b.push_back(uint8_t(value >> (8 * i)));
}
uint32_t string_at(Bytes &table, const std::string &name) {
    if (table.size() + name.size() >= UINT32_MAX) throw std::runtime_error("ELF string table too large");
    auto at = uint32_t(table.size());
    table.insert(table.end(), name.begin(), name.end());
    table.push_back(0);
    return at;
}
struct Section {
    std::string name;
    uint32_t type, link = 0, info = 0;
    uint64_t flags = 0, align = 1, entsize = 0, offset = 0, size = 0;
    Bytes data;
    Section(std::string n, uint32_t t) : name(std::move(n)), type(t) {}
};
enum { AMD64 = 62, AARCH64 = 183, RISCV = 243 };
unsigned reloc_type(unsigned machine, const sand_reloc_intern_t &r) {
    std::string name(r.type);
    if (name == "ABS") {
        if (machine == AMD64) {
            if (r.width_bits == 8) return 14;
            if (r.width_bits == 16) return 12;
            if (r.width_bits == 32) return 10;
            if (r.width_bits == 64) return 1;
        } else if (machine == AARCH64) {
            if (r.width_bits == 16) return 259;
            if (r.width_bits == 32) return 258;
            if (r.width_bits == 64) return 257;
        } else {
            if (r.width_bits == 32) return 1;
            if (r.width_bits == 64) return 2;
        }
    } else if (r.width_bits == 32) {
        if (machine == AMD64 && name == "R_X86_64_PC32") return 2;
        if (machine == AMD64 && name == "R_X86_64_PLT32") return 4;
        if (machine == AMD64 && name == "R_X86_64_GOTPCREL") return 9;
        if (machine == AARCH64) {
            if (name == "R_AARCH64_JUMP26") return 282;
            if (name == "R_AARCH64_CALL26") return 283;
            if (name == "R_AARCH64_ADR_PREL_PG_HI21") return 275;
            if (name == "R_AARCH64_ADR_PREL_LO21") return 274;
            if (name == "R_AARCH64_CONDBR19") return 280;
            if (name == "R_AARCH64_ADD_ABS_LO12_NC") return 277;
            if (name == "R_AARCH64_ADR_GOT_PAGE") return 311;
            if (name == "R_AARCH64_LD64_GOT_LO12_NC") return 312;
        }
        if (machine == RISCV && name == "R_RISCV_BRANCH") return 16;
        if (machine == RISCV && name == "R_RISCV_JAL") return 17;
        if (machine == RISCV && name == "R_RISCV_CALL_PLT") return 19;
        if (machine == RISCV && name == "R_RISCV_GOT_HI20") return 20;
        if (machine == RISCV && name == "R_RISCV_PCREL_HI20") return 23;
        if (machine == RISCV && name == "R_RISCV_PCREL_LO12_I") return 24;
    }
    throw std::runtime_error("unsupported ELF relocation: " + name + "/" + std::to_string(r.width_bits));
}
Bytes object(sandstone_asm_t *ctx) {
    const auto &target = *domsymas_target_at(ctx->bank, 0);
    std::string arch(target.arch ? target.arch : ""), abi(target.abi ? target.abi : "");
    unsigned machine = 0, flags = 0;
    if (arch == "amd64" && abi == "SysV") machine = AMD64;
    if (arch == "aarch64" && abi == "AAPCS64") machine = AARCH64;
    if (arch == "riscv64" && (abi == "LP64D" || abi == "LP64")) {
        machine = RISCV;
        flags = abi == "LP64D" ? 4 : 0;
        for (size_t i = 0; i < ctx->n_enabled; ++i)
            if (!strcmp(domsymas_feature_at(ctx->bank, ctx->enabled[i])->name, "c")) flags |= 1;
    }
    if (!machine || target.addr_size != 64 || target.byte_order || !target.obj_format ||
        strcmp(target.obj_format, "ELF64"))
        throw std::runtime_error("unsupported ELF target/ABI: " + arch + "/" + abi);
    std::vector<Section> sections{{"", 0}};
    sections[0].align = 0;
    for (size_t i = 0; i < ctx->n_sections; ++i) {
        const auto &s = ctx->sections[i];
        auto bank = domsymas_find_section(ctx->bank, s.name);
        uint32_t f = bank ? bank->flags : 0;
        if (!bank) {
            if (!strcmp(s.name, ".text")) f = DOMSYMAS_SF_ALLOC | DOMSYMAS_SF_EXEC;
            if (!strcmp(s.name, ".data") || !strcmp(s.name, ".bss"))
                f = DOMSYMAS_SF_ALLOC | DOMSYMAS_SF_WRITE;
            if (!strcmp(s.name, ".rodata")) f = DOMSYMAS_SF_ALLOC;
        }
        auto &sec = sections.emplace_back(s.name, s.is_bss ? 8 : 1);
        sec.flags = s.elf_flags_set
                        ? s.elf_flags
                        : ((f & DOMSYMAS_SF_ALLOC ? 2 : 0) | (f & DOMSYMAS_SF_WRITE ? 1 : 0) |
                           (f & DOMSYMAS_SF_EXEC ? 4 : 0) | (f & DOMSYMAS_SF_TLS ? 1024 : 0));
        sec.align = std::max(s.align, bank ? bank->align : 1u);
        if (!sec.align || (sec.align & (sec.align - 1)) || sec.align > 0x1000000)
            throw std::runtime_error("ELF section alignment must be a power of two up to 16 MiB");
        sec.size = s.len;
        if (!s.is_bss && s.len) sec.data.assign(s.bytes, s.bytes + s.len);
    }
    Bytes strings{0}, symbols(24);
    std::map<std::string, uint32_t> indices;
    struct Meta {
        unsigned type = 0;
        uint64_t size = 0;
    };
    std::map<std::string, Meta> meta;
    for (size_t i = 0; i < ctx->n_lines; ++i) {
        const auto &l = ctx->lines[i];
        if (l.kind != SAND_LINE_DIRECTIVE ||
            (strcmp(l.mnemonic, ".type") && strcmp(l.mnemonic, ".size")))
            continue;
        if (l.n_operands != 2)
            throw std::runtime_error("expected symbol and value for " + std::string(l.mnemonic));
        std::string name(l.operands[0].sval), arg(l.operands[1].sval);
        if (!strcmp(l.mnemonic, ".type")) {
            if (arg == "@function" || arg == "%function")
                meta[name].type = 2;
            else if (arg == "@object" || arg == "%object")
                meta[name].type = 1;
            else
                throw std::runtime_error("unsupported ELF symbol type: " + arg);
        } else {
            int64_t n;
            arg.erase(std::remove(arg.begin(), arg.end(), ' '), arg.end());
            auto s = sand_find_symbol(ctx, name.c_str());
            if (arg == ".-" + name && s && s->defined && s->sec_idx == l.section_idx &&
                l.address >= s->address)
                meta[name].size = l.address - s->address;
            else if (sand_parse_integer(arg.c_str(), &n) && n >= 0)
                meta[name].size = n;
            else
                throw std::runtime_error("unsupported ELF symbol size: " + arg);
        }
    }
    auto symbol = [&](const std::string &name, unsigned bind, unsigned sec, uint64_t val) {
        indices[name] = uint32_t(symbols.size() / 24);
        put(symbols, string_at(strings, name), 4);
        put(symbols, (bind << 4) | meta[name].type, 1);
        const auto* original = sand_find_symbol(ctx, name.c_str());
        put(symbols, original && original->hidden ? 2 : 0, 1);
        put(symbols, sec, 2);
        put(symbols, val, 8);
        put(symbols, meta[name].size, 8);
    };
    std::map<std::string,std::string> aliases;
    for (size_t i=0; i<ctx->n_lines; ++i) {
        const auto &l = ctx->lines[i];
        if (l.kind != SAND_LINE_DIRECTIVE || strcmp(l.mnemonic,".set") || l.n_operands != 2) continue;
        const char *a=l.operands[0].sval, *b=l.operands[1].sval;
        if (!sand_is_name(a) || !sand_is_name(b)) continue;
        auto t=sand_find_symbol(ctx,b);
        if (t && t->defined && !strcmp(t->section,"*abs*")) continue; // ordinary numeric .set
        auto s=sand_find_symbol(ctx,a);
        if ((s && s->defined) || !aliases.emplace(a,b).second)
            throw std::runtime_error("conflicting ELF alias: "+std::string(a));
    }
    for (auto &[a,b]:aliases)
        if (aliases.count(b)) throw std::runtime_error("chained or cyclic ELF alias: "+a);
    unsigned first_global = 0;
    for (unsigned bind : {0u, 1u}) {
        if (bind) first_global = uint32_t(symbols.size() / 24);
        for (size_t i = 0; i < ctx->n_symbols; ++i) {
            const auto &s = ctx->symbols[i];
            if (aliases.count(s.name)) continue;
            if (!s.defined && s.explicit_local)
                throw std::runtime_error("undefined local symbol: " + std::string(s.name));
            if (unsigned(s.global || !s.defined) != bind) continue;
            symbol(s.name, bind,
                   !s.defined                    ? 0
                   : !strcmp(s.section, "*abs*") ? 0xfff1
                                                 : s.sec_idx + 1,
                   s.address);
        }
        for (auto &[a,b]:aliases) {
            auto t=sand_find_symbol(ctx,b.c_str());
            auto s=sand_find_symbol(ctx,a.c_str());
            if (t && t->defined && unsigned(s && s->global)==bind) {
                if (!meta.count(a)) meta[a]=meta[b];
                symbol(a,bind,t->sec_idx+1,t->address);
            } else if ((!t || !t->defined) && s && s->global)
                throw std::runtime_error("cannot export alias of an undefined symbol: "+a);
        }
    }
    std::map<unsigned, unsigned> rela;
    for (size_t i = 0; i < ctx->n_relocs; ++i) {
        const auto &r = ctx->relocs[i];
        auto [ref, addend] = sand_object_ref(ctx, r);
        if (!indices.count(ref) && aliases.count(ref)) ref=aliases.at(ref);
        if (!indices.count(ref)) symbol(ref, 1, 0, 0);
        unsigned target_sec = r.sec_idx + 1;
        if (!rela.count(target_sec)) {
            rela[target_sec] = unsigned(sections.size());
            auto &s = sections.emplace_back(".rela" + sections[target_sec].name, 4);
            s.info = target_sec;
            s.align = 8;
            s.entsize = 24;
            s.flags = 64; // SHF_INFO_LINK
        }
        auto &data = sections[rela.at(target_sec)].data;
        unsigned type = reloc_type(machine, r);
        put(data, r.offset, 8);
        put(data, (uint64_t(indices.at(ref)) << 32) | type, 8);
        // x86 rel32 encoders count from the end of the displacement; ELF P
        // denotes its first byte.
        put(data, addend - (machine == AMD64 && (type == 2 || type == 4 || type == 9) ? 4 : 0), 8);
    }
    unsigned symidx = unsigned(sections.size());
    for (auto p : rela) sections[p.second].link = symidx;
    auto &symtab = sections.emplace_back(".symtab", 2);
    symtab.link = symidx + 1;
    symtab.info = first_global;
    symtab.entsize = 24;
    symtab.align = 8;
    symtab.data = std::move(symbols);
    sections.emplace_back(".strtab", 3).data = std::move(strings);
    sections.emplace_back(".shstrtab", 3);
    if (sections.size() >= 0xff00) throw std::runtime_error("too many ELF sections");
    Bytes names{0};
    std::vector<uint32_t> offsets{0};
    for (size_t i = 1; i < sections.size(); ++i) offsets.push_back(string_at(names, sections[i].name));
    sections.back().data = std::move(names);
    Bytes file(64);
    for (size_t i = 1; i < sections.size(); ++i) {
        auto &s = sections[i];
        s.offset = (file.size() + s.align - 1) & ~(s.align - 1);
        if (s.type == 8) continue;
        file.resize(s.offset);
        s.size = s.data.size();
        file.insert(file.end(), s.data.begin(), s.data.end());
    }
    file.resize((file.size() + 7) & ~size_t(7));
    Bytes hdr{0x7f, 'E', 'L', 'F', 2, 1, 1};
    hdr.resize(16);
    put(hdr, 1, 2);
    put(hdr, machine, 2);
    put(hdr, 1, 4);
    put(hdr, 0, 8);
    put(hdr, 0, 8);
    put(hdr, file.size(), 8);
    put(hdr, flags, 4);
    put(hdr, 64, 2);
    put(hdr, 0, 2);
    put(hdr, 0, 2);
    put(hdr, 64, 2);
    put(hdr, sections.size(), 2);
    put(hdr, sections.size() - 1, 2);
    std::copy(hdr.begin(), hdr.end(), file.begin());
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto &s = sections[i];
        put(file, offsets[i], 4);
        put(file, s.type, 4);
        put(file, s.flags, 8);
        put(file, 0, 8);
        put(file, s.offset, 8);
        put(file, s.size, 8);
        put(file, s.link, 4);
        put(file, s.info, 4);
        put(file, s.align, 8);
        put(file, s.entsize, 8);
    }
    return file;
}
} // namespace

extern "C" int sand_emit_elf64(sandstone_asm_t *ctx, const char *path) {
    try {
        auto bytes = object(ctx);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        out.close();
        if (!out) throw std::runtime_error("cannot write " + std::string(path));
        return 0;
    } catch (const std::exception &e) {
        sand_set_error(ctx, "%s", e.what());
        return -1;
    }
}
