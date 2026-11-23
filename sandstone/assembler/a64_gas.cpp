// Decode A64 GAS operands for forms carrying a64:* constraints. Opcode bits
// and relocation selections live in the bank, not in the syntax reader.
#include "intern.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {
using Text = std::string_view;
Text trim(Text s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}
bool prefix(Text &s, Text p) {
    if (s.substr(0, p.size()) != p)
        return false;
    s.remove_prefix(p.size());
    return true;
}
bool number(Text s, int64_t &out) {
    s = trim(s);
    prefix(s, "#");
    const bool neg = prefix(s, "-");
    if (!neg)
        prefix(s, "+");
    const bool hex = prefix(s, "0x") || prefix(s, "0X");
    if (s.empty())
        return false;
    uint64_t u;
    auto p = std::from_chars(s.data(), s.data() + s.size(), u, hex ? 16 : 10);
    if (p.ec != std::errc{} || p.ptr != s.data() + s.size() || (neg && u > (UINT64_C(1) << 63)) ||
        (!hex && !neg && u > INT64_MAX))
        return false;
    u = neg ? -u : u;
    std::memcpy(&out, &u, sizeof u);
    return true;
}
struct Operand {
    sand_op_kind_t kind = SAND_OP_IMM;
    int64_t value = 0, disp = 0;
    std::string symbol;
};
bool reg(sandstone_asm_t *ctx, Text constraint, Text s, Operand &op) {
    if (s.empty())
        return false;
    const auto name = std::string(s);
    auto r = domsymas_find_register(ctx->bank, name.c_str());
    if (!r)
        return false;
    const bool sp = s == "sp", zr = s == "xzr";
    if (constraint == "xsp" || constraint == "xbase") {
        if (!sp && (s.front() != 'x' || zr) && s != "fp" && s != "lr")
            return false;
    } else if (constraint == "x") {
        if (s.front() != 'x' && s != "fp" && s != "lr")
            return false;
    } else if (constraint.size() != 1 || constraint.front() != s.front() || s == "sp" || s == "wsp" ||
               s.size() < 2 || (!std::isdigit(static_cast<unsigned char>(s[1])) && s != "wzr"))
        return false;
    op.kind = SAND_OP_REG;
    op.value = r->enc_num;
    return true;
}
bool symbol(Text s, Operand &op) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
    op.symbol = std::string(s);
    if (!sand_is_name(op.symbol.c_str()))
        return false;
    op.kind = SAND_OP_SYM;
    return true;
}
bool match(sandstone_asm_t *ctx, const DomSymAs_Operand *desc, Text text, Operand &o) {
    Text c(desc && desc->constraint ? desc->constraint : "");
    if (!prefix(c, "a64:"))
        return false;
    Text s = trim(text);
    if (c == "sym")
        return symbol(s, o);
    if (c == "got" || c == "lo12")
        return prefix(s, c == "got" ? ":got:" : ":lo12:") && symbol(s, o);
    if (prefix(c, "mem")) {
        if (s.size() < 3 || s.front() != '[' || s.back() != ']')
            return false;
        s = s.substr(1, s.size() - 2);
        auto comma = s.find(',');
        if (!reg(ctx, "xbase", trim(s.substr(0, comma)), o))
            return false;
        Text index = comma == Text::npos ? Text{} : trim(s.substr(comma + 1));
        if (c == "got") {
            if (!prefix(index, ":got_lo12:") || !symbol(index, o))
                return false;
        } else {
            int64_t scale;
            if (!number(c, scale) || scale <= 0)
                return false;
            if (comma != Text::npos && !number(index, o.disp))
                return false;
            if (o.disp < 0 || o.disp % scale || o.disp / scale > 4095)
                return false;
        }
        o.kind = SAND_OP_MEM;
        return true;
    }
    if (c == "cond") {
        static constexpr Text cc[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs",
                                      "vc", "hi", "ls", "ge", "lt", "gt", "le"};
        if (s == "cs")
            s = "hs";
        if (s == "cc")
            s = "lo";
        auto i = std::find(std::begin(cc), std::end(cc), s);
        if (i == std::end(cc))
            return false;
        o.kind = SAND_OP_COND;
        o.value = i - cc;
        return true;
    }
    if (c == "uxtx")
        return s == "uxtx";
    if (c == "lsl" || c == "asr") {
        if (!prefix(s, c) || s.empty() || !std::isspace(static_cast<unsigned char>(s.front())))
            return false;
        return number(s, o.value);
    }
    if (c == "imm")
        return number(s, o.value);
    return reg(ctx, c, s, o);
}
} // namespace

extern "C" int sand_a64_operands(sandstone_asm_t *ctx, sand_line_t *line, char **ops, size_t count) try {
    auto t = domsymas_target_at(ctx->bank, 0);
    if (!t || std::strcmp(t->arch, "aarch64"))
        return 0;
    uint32_t forms[64], n = 0;
    // If the bank owns this mnemonic, never reinterpret a bad addressing
    // mode as an unresolved symbol or silently lose the operand's width.
    if (domsymas_find_mnemonic(ctx->bank, line->mnemonic, forms, 64, &n) != DOMSYMAS_OK)
        return 0;
    bool typed = false;
    for (unsigned i = 0; i < n; ++i) {
        auto d = domsymas_operand_at(ctx->bank, forms[i], 0);
        if (d && d->constraint && Text(d->constraint).substr(0, 4) == "a64:")
            typed = true;
    }
    if (!typed)
        return 0;
    if (count > 4 || domsymas_resolve(ctx->bank, line->mnemonic, count, ctx->enabled, ctx->n_enabled, forms,
                                      64, &n) != DOMSYMAS_OK)
        return -1;
    for (unsigned i = 0; i < n; ++i) {
        if (domsymas_operand_count(ctx->bank, forms[i]) != count)
            continue;
        std::array<Operand, 4> parsed;
        bool valid = true;
        for (unsigned j = 0; j < count && valid; ++j)
            valid = match(ctx, domsymas_operand_at(ctx->bank, forms[i], j), ops[j], parsed[j]);
        if (!valid)
            continue;
        auto *out = static_cast<sand_operand_t *>(std::calloc(count, sizeof(sand_operand_t)));
        if (count && !out)
            return -1;
        for (unsigned j = 0; j < count; ++j) {
            out[j].kind = parsed[j].kind;
            out[j].ival = parsed[j].value;
            if (!parsed[j].symbol.empty() && !(out[j].sval = sand_strdup(parsed[j].symbol.c_str()))) {
                for (unsigned k = 0; k < j; ++k)
                    std::free(out[k].sval);
                std::free(out);
                return -1;
            }
            if (out[j].kind == SAND_OP_MEM)
                line->a64_disp = parsed[j].disp;
        }
        line->operands = out;
        line->n_operands = count;
        line->a64_form_plus_one = forms[i] + 1;
        return 1;
    }
    return -1;
} catch (const std::bad_alloc &) {
    return -1;
}

extern "C" int sand_a64_bindings(const sand_line_t *line, DomSymAs_OpVal **vals_out, uint32_t *n_out) {
    static constexpr const char *pos[] = {"r0", "r1", "r2", "r3"};
    if (line->n_operands > 4)
        return 0;
    auto *args = static_cast<DomSymAs_OpVal *>(std::calloc(2 * line->n_operands + 1, sizeof(DomSymAs_OpVal)));
    if (!args)
        return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < line->n_operands; ++i) {
        args[n].name = pos[i];
        args[n++].ival = line->operands[i].ival;
        if (line->operands[i].sval) {
            args[n].name = "sym";
            args[n].is_symbol = 1;
            args[n++].sval = line->operands[i].sval;
        }
    }
    args[n].name = "disp";
    args[n++].ival = line->a64_disp;
    *vals_out = args;
    *n_out = n;
    return 1;
}

// Same-section branches to non-preemptible labels (or flat image labels).
// Leave all page/GOT references to the ELF linker, where P and GOT are known.
extern "C" int sand_a64_patch_local(uint8_t *bytes, int64_t delta, const char *name) {
    Text kind(name);
    unsigned bits, shift;
    if (kind == "R_AARCH64_CONDBR19") {
        bits = 19;
        shift = 5;
    } else if (kind == "R_AARCH64_JUMP26" || kind == "R_AARCH64_CALL26") {
        bits = 26;
        shift = 0;
    } else
        return 0;
    const int64_t half = INT64_C(1) << (bits + 1);
    if (delta % 4 || delta < -half || delta >= half)
        return -1;
    const uint32_t field = (static_cast<uint32_t>(delta / 4) & ((uint32_t(1) << bits) - 1)) << shift;
    for (unsigned j = 0; j < 4; ++j)
        bytes[j] |= static_cast<uint8_t>(field >> (8 * j));
    return 1;
}
