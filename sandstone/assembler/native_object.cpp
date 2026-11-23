// Windows COFF and Darwin Mach-O relocatable objects. All offsets and
// integers are serialized explicitly; no host struct layout or target SDK.
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
using std::string;
void require(bool condition, const string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
uint32_t u32(uint64_t x) {
    require(x <= UINT32_MAX, "object exceeds 32-bit field limit");
    return uint32_t(x);
}
uint64_t aligned(uint64_t n, unsigned a) { return (n + a - 1) & -uint64_t(a); }
void put(Bytes &b, uint64_t v, unsigned n) {
    while (n--) {
        b.push_back(uint8_t(v));
        v >>= 8;
    }
}
void fixed(Bytes &b, const string &s, unsigned n) {
    require(s.size() <= n, "section or symbol name too long: " + s);
    b.insert(b.end(), s.begin(), s.end());
    b.resize(b.size() + n - s.size());
}
void append(Bytes &b, const Bytes &v) { b.insert(b.end(), v.begin(), v.end()); }
uint32_t str(Bytes &b, const string &s) {
    auto at = u32(b.size());
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
    return at;
}
struct Reloc {
    uint32_t offset, index, type, width;
    bool pc;
};
struct Section {
    string name, segment;
    unsigned power = 0;
    bool code, write, bss;
    uint32_t size, file = 0, rel = 0;
    uint64_t addr = 0;
    Bytes data;
    std::vector<Reloc> relocs;
};
struct Symbol {
    string name;
    int section;
    uint64_t value;
    bool global;
};
struct Model {
    bool coff, arm;
    std::vector<Section> sections;
    std::vector<Symbol> symbols;
};

Model collect(sandstone_asm_t *ctx, bool coff) {
    const auto &target = *domsymas_target_at(ctx->bank, 0);
    string arch = target.arch ? target.arch : "", abi = target.abi ? target.abi : "";
    string fmt = target.obj_format ? target.obj_format : "";
    string cc = target.calling_conv ? target.calling_conv : "";
    Model m{coff, arch == "aarch64", {}, {}};
    require((m.arm || arch == "amd64") && target.addr_size == 64 && !target.byte_order &&
                abi == (coff ? "Win64" : "Darwin") && fmt == (coff ? "COFF" : "Mach-O") &&
                cc == (coff ? "windows-" : "darwin-") + arch,
            "unsupported object target/ABI: " + arch + "/" + abi + "/" + fmt + "/" + cc);
    require(ctx->n_sections <= (coff ? 32767u : 255u), "too many object sections");
    for (size_t i = 0; i < ctx->n_sections; ++i) {
        const auto &s = ctx->sections[i];
        Section sec{};
        string name(s.name);
        require(name == ".text" || name == ".data" || name == ".rodata" || name == ".bss",
                "unsupported native object section: " + name);
        sec.code = name == ".text";
        sec.write = name == ".data" || name == ".bss";
        sec.bss = name == ".bss";
        require(sec.bss == bool(s.is_bss) &&
                    (!s.elf_flags_set || s.elf_flags == (2 | (sec.code ? 4 : 0) | (sec.write ? 1 : 0))),
                "conflicting native section attributes: " + name);
        sec.name = coff ? (name == ".rodata" ? ".rdata" : name)
                        : (name == ".rodata" ? "__const" : "__" + name.substr(1));
        sec.segment = sec.write ? "__DATA" : "__TEXT";
        auto bank = domsymas_find_section(ctx->bank, s.name);
        unsigned a = std::max(s.align, bank ? bank->align : 1u);
        require(a && !(a & (a - 1)) && a <= (coff ? 8192u : 1u << 24),
                "unsupported native section alignment: " + std::to_string(a));
        while ((1u << sec.power) < a)
            ++sec.power;
        sec.size = u32(s.len);
        if (!sec.bss && s.len)
            sec.data.assign(s.bytes, s.bytes + s.len);
        m.sections.push_back(std::move(sec));
    }
    std::map<string, uint32_t> indices;
    auto symbol = [&](const string &name, int section, uint64_t value, bool global) {
        auto index = u32(m.symbols.size());
        indices.emplace(name, index);
        m.symbols.push_back({name, section, value, global});
        return index;
    };
    for (size_t i = 0; i < ctx->n_lines; ++i) {
        const auto &l = ctx->lines[i];
        if (l.kind != SAND_LINE_DIRECTIVE)
            continue;
        if (!strcmp(l.mnemonic, ".set") && l.n_operands == 2) {
            auto s = sand_find_symbol(ctx, l.operands[0].sval);
            require(s && s->defined && !strcmp(s->section, "*abs*"), "unsupported native .set alias");
        }
    }
    for (size_t i = 0; i < ctx->n_symbols; ++i) {
        const auto &s = ctx->symbols[i];
        require(!s.hidden, "unsupported native .hidden symbol: " + string(s.name));
        require(s.defined || !s.explicit_local, "undefined local symbol: " + string(s.name));
        symbol(s.name,
               !s.defined                    ? 0
               : !strcmp(s.section, "*abs*") ? -1
                                             : int(s.sec_idx) + 1,
               s.address, s.global || !s.defined);
    }
    for (size_t i = 0; i < ctx->n_relocs; ++i) {
        const auto &r = ctx->relocs[i];
        auto &sec = m.sections.at(r.sec_idx);
        string name(r.type);
        auto [ref, add] = sand_object_ref(ctx, r);
        unsigned width = r.width_bits / 8, type;
        bool pc = false, instruction = false;
        if (name == "ABS" && (r.width_bits == 64 || (coff && r.width_bits == 32))) {
            type = !coff ? 0 : m.arm ? (width == 8 ? 14 : 1) : (width == 8 ? 1 : 2);
        } else if (!m.arm && r.width_bits == 32 &&
                   (name == "R_X86_64_PLT32" || name == "R_X86_64_PC32")) {
            pc = true;
            // The bank also uses PC32 for near Jcc (0F 80..8F). Darwin
            // distinguishes a branch to a linker's stub from a data reference.
            bool branch = name == "R_X86_64_PLT32" ||
                          (r.offset >= 2 && r.offset <= sec.data.size() &&
                           sec.data[r.offset-2] == 0x0f && (sec.data[r.offset-1] & 0xf0) == 0x80);
            type = coff ? 4 : branch ? 2 : 1;
        } else if (m.arm && r.width_bits == 32 &&
                   (name == "R_AARCH64_CALL26" || name == "R_AARCH64_JUMP26" ||
                    name == "R_AARCH64_ADR_PREL_PG_HI21" || name == "R_AARCH64_ADD_ABS_LO12_NC")) {
            bool page = name == "R_AARCH64_ADR_PREL_PG_HI21", low = name == "R_AARCH64_ADD_ABS_LO12_NC";
            pc = !low;
            type = page ? (coff ? 4 : 3) : low ? (coff ? 6 : 4) : (coff ? 3 : 2);
            instruction = true;
            require(!add && !(r.offset & 3),
                    "unsupported AArch64 instruction addend or alignment: " + name);
        } else
            throw std::runtime_error("unsupported native relocation: " + name + "/" +
                                     std::to_string(r.width_bits));
        require(uint64_t(r.offset) + width <= sec.data.size(), "relocation outside section: " + name);
        if (!instruction) {
            if (width == 4)
                require(int64_t(add) >= INT32_MIN &&
                            int64_t(add) <= (pc ? int64_t(INT32_MAX) : int64_t(UINT32_MAX)),
                        "relocation addend overflow");
            for (unsigned j = 0; j < width; ++j)
                sec.data[r.offset + j] = uint8_t(add >> (8 * j));
        }
        auto index = indices.count(ref) ? indices.at(ref) : symbol(ref, 0, 0, true);
        sec.relocs.push_back({r.offset, index, type, width, pc});
    }
    return m;
}

Bytes coff_object(Model &m) {
    Bytes file(20 + 40 * m.sections.size()), hdr, strings(4);
    for (auto &s : m.sections) {
        if (!s.data.empty()) {
            file.resize(aligned(file.size(), 4));
            s.file = u32(file.size());
            append(file, s.data);
        }
        require(s.relocs.size() < 65535, "COFF relocation overflow records unsupported");
        if (!s.relocs.empty())
            s.rel = u32(file.size());
        for (auto r : s.relocs) {
            put(file, r.offset, 4);
            put(file, r.index, 4);
            put(file, r.type, 2);
        }
    }
    put(hdr, m.arm ? 0xaa64 : 0x8664, 2);
    put(hdr, m.sections.size(), 2);
    put(hdr, 0, 4);
    put(hdr, u32(file.size()), 4);
    put(hdr, u32(m.symbols.size()), 4);
    put(hdr, 0, 4);
    for (const auto &s : m.sections) {
        fixed(hdr, s.name, 8);
        put(hdr, 0, 8);
        put(hdr, s.size, 4);
        put(hdr, s.file, 4);
        put(hdr, s.rel, 4);
        put(hdr, 0, 4);
        put(hdr, s.relocs.size(), 2);
        put(hdr, 0, 2);
        uint32_t flags = 0x40000000 | ((s.power + 1) << 20); // MEM_READ | ALIGN
        flags |= s.code ? 0x20000020 : s.bss ? 0x80 : 0x40;
        if (s.write)
            flags |= 0x80000000;
        put(hdr, flags, 4);
    }
    for (const auto &s : m.symbols) {
        if (s.name.size() <= 8)
            fixed(file, s.name, 8);
        else {
            put(file, 0, 4);
            put(file, str(strings, s.name), 4);
        }
        put(file, u32(s.value), 4);
        put(file, s.section, 2);
        put(file, 0, 2);
        put(file, s.global ? 2 : 3, 1);
        put(file, 0, 1);
    }
    Bytes len;
    put(len, u32(strings.size()), 4);
    std::copy(len.begin(), len.end(), strings.begin());
    append(file, strings);
    std::copy(hdr.begin(), hdr.end(), file.begin());
    return file;
}

Bytes macho_object(Model &m) {
    // LC_SEGMENT_64, LC_SYMTAB, LC_BUILD_VERSION (macOS 11).
    unsigned segsize = 72 + 80 * m.sections.size(), commands = segsize + 24 + 24;
    uint32_t start = 32 + commands;
    unsigned maxalign = 1;
    for (const auto &s : m.sections)
        if (!s.bss)
            maxalign = std::max(maxalign, 1u << s.power);
    start = u32(aligned(start, maxalign));
    Bytes file(start), hdr, strings{0};
    uint64_t vm = 0;
    // The Mach section table (and its one-based n_sect indices) must put
    // zero-fill after file-backed sections even when .bss appeared first.
    std::vector<Section *> order;
    std::vector<unsigned> ordinals(m.sections.size());
    for (bool bss : {false, true})
        for (size_t i = 0; i < m.sections.size(); ++i) {
            auto &s = m.sections[i];
            if (s.bss != bss)
                continue;
            order.push_back(&s);
            ordinals[i] = unsigned(order.size());
            s.addr = aligned(vm, 1u << s.power);
            vm = s.addr + s.size;
            if (!bss) {
                s.file = u32(start + s.addr);
                file.resize(s.file);
                append(file, s.data);
            }
        }
    uint64_t filesize = file.size() - start;
    file.resize(aligned(file.size(), 8));
    for (auto &s : m.sections) {
        if (!s.relocs.empty())
            s.rel = u32(file.size());
        std::stable_sort(s.relocs.begin(), s.relocs.end(),
                         [](auto a, auto b) { return a.offset > b.offset; });
        for (auto r : s.relocs) {
            require(r.index < (1u << 24) && r.offset <= INT32_MAX, "Mach-O relocation field overflow");
            put(file, r.offset, 4);
            put(file,
                r.index | (unsigned(r.pc) << 24) | ((r.width == 8 ? 3u : 2u) << 25) | (1u << 27) |
                    (r.type << 28),
                4);
        }
    }
    uint32_t symoff = u32(file.size());
    for (const auto &s : m.symbols) {
        put(file, str(strings, s.name), 4);
        put(file, (s.section > 0 ? 14 : s.section < 0 ? 2 : 0) | unsigned(s.global), 1);
        put(file, s.section > 0 ? ordinals.at(s.section - 1) : 0, 1);
        put(file, 0, 2);
        put(file, s.value + (s.section > 0 ? m.sections.at(s.section - 1).addr : 0), 8);
    }
    put(hdr, 0xfeedfacf, 4);
    put(hdr, m.arm ? 0x100000c : 0x1000007, 4);
    put(hdr, m.arm ? 0 : 3, 4);
    put(hdr, 1, 4);
    put(hdr, 3, 4);
    put(hdr, commands, 4);
    put(hdr, 0, 8);
    put(hdr, 0x19, 4);
    put(hdr, segsize, 4);
    fixed(hdr, "", 16);
    put(hdr, 0, 8);
    put(hdr, vm, 8);
    put(hdr, start, 8);
    put(hdr, filesize, 8);
    put(hdr, 7, 4);
    put(hdr, 7, 4);
    put(hdr, m.sections.size(), 4);
    put(hdr, 0, 4);
    for (const auto *section : order) {
        const auto &s = *section;
        fixed(hdr, s.name, 16);
        fixed(hdr, s.segment, 16);
        put(hdr, s.addr, 8);
        put(hdr, s.size, 8);
        put(hdr, s.file, 4);
        put(hdr, s.power, 4);
        put(hdr, s.rel, 4);
        put(hdr, u32(s.relocs.size()), 4);
        put(hdr, s.code ? 0x80000400 : s.bss ? 1 : 0, 4);
        put(hdr, 0, 8);
        put(hdr, 0, 4);
    }
    put(hdr, 2, 4);
    put(hdr, 24, 4);
    put(hdr, symoff, 4);
    put(hdr, u32(m.symbols.size()), 4);
    put(hdr, u32(file.size()), 4);
    put(hdr, u32(strings.size()), 4);
    put(hdr, 0x32, 4);
    put(hdr, 24, 4);
    put(hdr, 1, 4);
    put(hdr, 11 << 16, 4);
    put(hdr, 0, 8);
    append(file, strings);
    std::copy(hdr.begin(), hdr.end(), file.begin());
    return file;
}
} // namespace

extern "C" int sand_emit_native_object(sandstone_asm_t *ctx, const char *path, const char *format) {
    try {
        auto m = collect(ctx, !strcmp(format, "coff"));
        auto bytes = m.coff ? coff_object(m) : macho_object(m);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        out.close();
        require(bool(out), "cannot write " + string(path));
        return 0;
    } catch (const std::exception &e) {
        sand_set_error(ctx, "%s", e.what());
        return -1;
    }
}
