#include "qml_rewrite.hpp"

#include "cephyr/diagnostics.hpp"
#include "mir_verify.h"

#include <NativeInternal.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>

namespace cephyr::qml {
namespace {

constexpr std::size_t max_compiled_bytes = 1024 * 1024;
constexpr std::size_t max_rule_line = 4096;
constexpr std::size_t max_phases = 64;
constexpr std::size_t max_rules = 1024;
constexpr unsigned max_fuel = 1'000'000;
constexpr unsigned max_attempts = 16'000'000;

enum class Op : unsigned char {
    opaque, const_, trunc, zext, sext, copy, add, sub, mul, and_, or_, xor_,
    shl, lshr, ashr, eq, slt, ult, count
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Op::count)> op_names = {
    "", "const", "trunc", "zext", "sext", "copy", "add", "sub", "mul", "and", "or",
    "xor", "shl", "lshr", "ashr", "eq", "slt", "ult"
};

constexpr std::size_t index(Op op) { return static_cast<std::size_t>(op); }

struct Opcode {
    std::string_view name;
    Op op = Op::opaque;
    unsigned fields = 0;
};

// An opaque record still needs its line width checked. The native reader
// ignores newlines; a symbol such as "add" in a split call record must never
// be mistaken for an editable opcode.
constexpr unsigned direct_call = 4;
constexpr unsigned indirect_call = 5;
constexpr Opcode opaque_opcodes[] = {
    {"call", Op::opaque, direct_call}, {"call_indirect", Op::opaque, indirect_call},
    {"fconst", Op::opaque, 2}, {"frame_addr", Op::opaque, 2}, {"global_addr", Op::opaque, 2},
    {"func_addr", Op::opaque, 2}, {"ptr_to_int", Op::opaque, 2}, {"int_to_ptr", Op::opaque, 2},
    {"func_ptr_to_int", Op::opaque, 2}, {"int_to_func_ptr", Op::opaque, 2},
    {"fneg", Op::opaque, 2}, {"fcast", Op::opaque, 2}, {"sitofp", Op::opaque, 2},
    {"uitofp", Op::opaque, 2}, {"fptosi", Op::opaque, 2}, {"fptoui", Op::opaque, 2},
    {"div_s", Op::opaque, 3}, {"div_u", Op::opaque, 3}, {"rem_s", Op::opaque, 3},
    {"rem_u", Op::opaque, 3}, {"add_checked_s", Op::opaque, 3}, {"sub_checked_s", Op::opaque, 3},
    {"mul_checked_s", Op::opaque, 3}, {"div_checked_s", Op::opaque, 3}, {"ptr_add", Op::opaque, 3},
    {"load", Op::opaque, 3}, {"load_ptr", Op::opaque, 3}, {"load_func_ptr", Op::opaque, 3},
    {"load_s8", Op::opaque, 3}, {"load_s16", Op::opaque, 3}, {"load_s32", Op::opaque, 3},
    {"load_u8", Op::opaque, 3}, {"load_u16", Op::opaque, 3}, {"load_u32", Op::opaque, 3},
    {"store", Op::opaque, 3}, {"store_ptr", Op::opaque, 3}, {"store_func_ptr", Op::opaque, 3},
    {"store8", Op::opaque, 3}, {"store16", Op::opaque, 3}, {"store32", Op::opaque, 3},
    {"fload", Op::opaque, 3}, {"fstore", Op::opaque, 3}, {"fadd", Op::opaque, 3},
    {"fsub", Op::opaque, 3}, {"fmul", Op::opaque, 3}, {"fdiv", Op::opaque, 3},
    {"feq", Op::opaque, 3}, {"flt", Op::opaque, 3}, {"fle", Op::opaque, 3}
};

constexpr std::size_t opcode_slots = 128;
static_assert(op_names.size() + std::size(opaque_opcodes) < opcode_slots);

constexpr std::size_t opcode_hash(std::string_view text)
{
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : text)
        hash = (hash ^ c) * 16777619u;
    return hash % opcode_slots;
}

constexpr auto opcode_table = [] {
    std::array<Opcode, opcode_slots> table{};
    const auto insert = [&](Opcode op) {
        auto slot = opcode_hash(op.name);
        while (!table[slot].name.empty())
            slot = (slot + 1) % table.size();
        table[slot] = op;
    };
    for (std::size_t i = 1; i < op_names.size(); ++i)
        insert({op_names[i], static_cast<Op>(i)});
    for (auto op : opaque_opcodes)
        insert(op);
    return table;
}();

constexpr std::size_t max_opcode_length = [] {
    std::size_t max = 0;
    for (const auto &op : opcode_table)
        max = std::max(max, op.name.size());
    return max;
}();

const Opcode *find_opcode(std::string_view text)
{
    if (text.empty() || text.size() > max_opcode_length)
        return nullptr;
    auto slot = opcode_hash(text);
    while (!opcode_table[slot].name.empty()) {
        if (opcode_table[slot].name == text)
            return &opcode_table[slot];
        slot = (slot + 1) % opcode_table.size();
    }
    return nullptr;
}

bool rule_op(Op op) { return op >= Op::copy; }
unsigned arity(Op op) { return op == Op::copy ? 1 : 2; }

struct Tokens {
    std::string_view rest;

    std::string_view take()
    {
        if (rest.empty())
            return {};
        const auto space = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; };
        auto cursor = rest.data();
        const auto end = cursor + rest.size();
        while (cursor != end && space(*cursor))
            ++cursor;
        const auto begin = cursor;
        while (cursor != end && !space(*cursor))
            ++cursor;
        rest = {cursor, static_cast<std::size_t>(end - cursor)};
        return {begin, static_cast<std::size_t>(cursor - begin)};
    }

    bool skip(std::size_t count)
    {
        while (count--)
            if (take().empty())
                return false;
        return true;
    }

    bool done() { return take().empty(); }
};

struct Lines {
    std::string_view text;
    std::size_t offset = 0;
    unsigned line = 0;

    bool take(std::string_view &out)
    {
        if (offset == text.size())
            return false;
        auto end = text.find('\n', offset);
        if (end == text.npos)
            end = text.size();
        out = text.substr(offset, end - offset);
        offset = end + (end != text.size());
        ++line;
        return true;
    }
};

template <typename T>
bool integer(std::string_view text, T &number, int base = 10)
{
    if (text.empty())
        return false;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, number, base);
    return result.ec == std::errc{} && result.ptr == end;
}

bool literal(std::string_view text, std::uint64_t &bits)
{
    if (text.starts_with('-')) {
        std::int64_t signed_word;
        if (!integer(text, signed_word))
            return false;
        bits = static_cast<std::uint64_t>(signed_word);
        return true;
    }
    return integer(text, bits);
}

bool mir_word(std::string_view text, std::uint64_t &word)
{
    return text.starts_with("0x") ? integer(text.substr(2), word, 16) : integer(text, word);
}

bool identifier(std::string_view text)
{
    const auto head = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    return !text.empty() && text.size() <= 255 && head(text.front()) &&
           std::all_of(text.begin() + 1, text.end(),
                       [&](char c) { return head(c) || (c >= '0' && c <= '9'); });
}

struct Match {
    enum Kind : unsigned char { any, same, bits } kind = any;
    std::uint64_t word = 0; // For same, the index of an earlier operand.
};

struct Rule {
    std::string name;
    std::array<Match, 2> operands;
    // opaque means a rejection; const_ means a literal replacement. Any
    // other replacement draws only from the operands of the original root.
    Op replacement = Op::opaque;
    std::array<unsigned, 2> slots{};
    std::uint64_t word = 0;
};

using RulesByOp = std::array<std::vector<Rule>, op_names.size()>;

struct Phase {
    std::string name;
    RulesByOp rewrites;
    RulesByOp checks;
    bool has_rewrites = false;
    bool has_checks = false;
};

bool root(Tokens &tokens, Op &op)
{
    const auto entry = find_opcode(tokens.take());
    op = entry ? entry->op : Op::opaque;
    unsigned count;
    return rule_op(op) && integer(tokens.take(), count) && count == arity(op);
}

bool parse_rule(Tokens &tokens, bool reject, Op &op, Rule &rule, std::string_view &error)
{
    std::array<std::string_view, 2> metas{};
    if (tokens.take() != "n" || !root(tokens, op)) {
        error = "expected a supported native i64 root operator with its exact arity";
        return false;
    }
    for (unsigned i = 0; i < arity(op); ++i) {
        const auto tag = tokens.take();
        if (tag == "w")
            continue;
        if (tag == "m") {
            metas[i] = tokens.take();
            if (identifier(metas[i])) {
                if (i != 0 && metas[i] == metas[0])
                    rule.operands[i] = {Match::same, 0};
                continue;
            }
        } else if (tag == "i" && literal(tokens.take(), rule.operands[i].word)) {
            rule.operands[i].kind = Match::bits;
            continue;
        }
        error = "native patterns require flat metavariable, integer or wildcard operands";
        return false;
    }

    const auto source_slot = [&](std::string_view tag, std::string_view value, unsigned &slot) {
        std::uint64_t word = 0;
        if (tag != "m" && (tag != "i" || !literal(value, word)))
            return false;
        for (unsigned i = 0; i < arity(op); ++i) {
            if ((tag == "m" && !value.empty() && value == metas[i]) ||
                (tag == "i" && rule.operands[i].kind == Match::bits &&
                 word == rule.operands[i].word)) {
                slot = i;
                return true;
            }
        }
        return false;
    };

    if (!reject) {
        const auto tag = tokens.take();
        if (tag == "m") {
            rule.replacement = Op::copy;
            if (!source_slot(tag, tokens.take(), rule.slots[0])) {
                error = "unbound replacement metavariable";
                return false;
            }
        } else if (tag == "i") {
            rule.replacement = Op::const_;
            if (!literal(tokens.take(), rule.word)) {
                error = "replacement integer does not fit an i64 bit pattern";
                return false;
            }
        } else if (tag == "n" && root(tokens, rule.replacement)) {
            for (unsigned i = 0; i < arity(rule.replacement); ++i) {
                const auto child_tag = tokens.take();
                if (!source_slot(child_tag, tokens.take(), rule.slots[i])) {
                    error = "replacement operands must be bound metavariables or matched integers";
                    return false;
                }
            }
        } else {
            error = "expected a supported flat native i64 replacement, integer or metavariable";
            return false;
        }
    }
    if (!tokens.done()) {
        error = "unexpected tokens after native rule";
        return false;
    }
    return true;
}

struct Inst {
    Op op = Op::opaque;
    std::array<unsigned, 2> use{};
    std::uint64_t word = 0;
    bool operator==(const Inst &) const = default;
};

struct Site {
    Inst original;
    Inst inst;
    unsigned dst;
    unsigned line;
    std::size_t offset;
    std::size_t length;
};

struct Block {
    std::vector<Site> sites;
};

struct Function {
    unsigned value_count = 0;
    std::vector<Block> blocks;
};

bool opaque_instruction(unsigned fields, Tokens &tokens)
{
    if (fields == direct_call || fields == indirect_call) {
        unsigned count;
        return tokens.skip(fields == direct_call ? 2 : 3) && integer(tokens.take(), count) &&
               count <= AN_MAX_PARAMS && tokens.skip(count) && tokens.done();
    }
    return tokens.skip(fields) && tokens.done();
}

bool instruction(Lines &lines, Block &block, const std::vector<unsigned char> &i64)
{
    const auto offset = lines.offset;
    std::string_view line;
    if (!lines.take(line))
        return false;
    Tokens tokens{line};
    const auto entry = find_opcode(tokens.take());
    if (!entry)
        return false;
    Inst inst{entry->op};
    if (inst.op == Op::opaque)
        return opaque_instruction(entry->fields, tokens);

    const auto value = [&](unsigned &id) {
        return integer(tokens.take(), id) && id < i64.size();
    };
    unsigned dst;
    if (!value(dst))
        return false;
    if (inst.op != Op::const_ && !value(inst.use[0]))
        return false;
    if (inst.op > Op::copy && !value(inst.use[1]))
        return false;
    if (inst.op < Op::copy && !mir_word(tokens.take(), inst.word))
        return false;
    if (!tokens.done())
        return false;
    if (inst.op == Op::copy && !i64[dst])
        return true; // Valid noninteger copies are opaque, too.
    if (!i64[dst] || (inst.op != Op::const_ && !i64[inst.use[0]]) ||
        (inst.op > Op::copy && !i64[inst.use[1]]) ||
        (inst.op < Op::copy && inst.op != Op::const_ && (inst.word < 1 || inst.word > 64)))
        return false;
    block.sites.push_back({inst, inst, dst, lines.line, offset,
                           line.size() - line.ends_with('\r')});
    return true;
}

bool terminator(Tokens &tokens)
{
    const auto edge = [&] {
        unsigned count;
        return tokens.skip(1) && integer(tokens.take(), count) && tokens.skip(count);
    };
    const auto op = tokens.take();
    if (op == "br")
        return edge() && tokens.done();
    if (op == "cbr")
        return tokens.skip(1) && edge() && edge() && tokens.done();
    if (op == "ret")
        return tokens.skip(1) && tokens.done();
    return op == "trap" && tokens.done();
}

// Read only the canonical or already-verified text emitted by lower.cpp. All
// non-participating records are preserved unchanged, including branch args,
// calls, frame declarations, imports and globals.
bool read_module(Lines &lines, std::vector<Function> &module)
{
    const auto next = [&] {
        std::string_view line;
        lines.take(line);
        return Tokens{line};
    };
    auto header = next();
    if (header.take() != "archon-mir" || header.take() != "1" || !header.done())
        return false;
    auto module_line = next();
    unsigned functions, imports, globals;
    if (module_line.take() != "module" || !integer(module_line.take(), functions) ||
        !integer(module_line.take(), imports) || !integer(module_line.take(), globals) ||
        !module_line.done() || functions > AN_MAX_SYMBOLS ||
        imports > AN_MAX_SYMBOLS - functions || globals > AN_MAX_SYMBOLS - functions - imports)
        return false;
    module.reserve(functions);
    unsigned module_instructions = 0;
    for (unsigned f = 0; f < functions; ++f) {
        auto function = next();
        unsigned locals, blocks;
        auto &fn = module.emplace_back();
        if (function.take() != "function" || !function.skip(3) ||
            !integer(function.take(), fn.value_count) || !integer(function.take(), blocks) ||
            !integer(function.take(), locals) || !function.done() ||
            fn.value_count > AN_MAX_VALUE || blocks > AN_MAX_BLOCK || locals > AN_MAX_VALUE)
            return false;
        auto types = next();
        if (types.take() != "types")
            return false;
        std::vector<unsigned char> i64;
        i64.reserve(fn.value_count);
        for (unsigned i = 0; i < fn.value_count; ++i) {
            const auto type = types.take();
            if (type.empty())
                return false;
            i64.push_back(type == "i64");
        }
        if (!types.done())
            return false;
        for (unsigned i = 0; i < locals; ++i) {
            auto local = next();
            if (local.take() != "local" || !local.skip(2) || !local.done())
                return false;
        }
        fn.blocks.reserve(blocks);
        unsigned function_instructions = 0;
        for (unsigned b = 0; b < blocks; ++b) {
            auto header = next();
            unsigned params, count;
            if (header.take() != "block" || !integer(header.take(), params) ||
                !integer(header.take(), count) || !header.done() || params > fn.value_count ||
                count > AN_MAX_INSTS - function_instructions ||
                count > AN_MAX_MODULE_INSTS - module_instructions)
                return false;
            function_instructions += count;
            module_instructions += count;
            auto parameters = next();
            if (parameters.take() != "params" || !parameters.skip(params) || !parameters.done())
                return false;
            auto &block = fn.blocks.emplace_back();
            block.sites.reserve(std::min(count, fn.value_count));
            for (unsigned i = 0; i < count; ++i)
                if (!instruction(lines, block, i64))
                    return false;
            auto term = next();
            if (!terminator(term))
                return false;
        }
    }
    for (unsigned i = 0; i < imports; ++i) {
        auto import = next();
        unsigned params;
        if (import.take() != "import" || !import.skip(2) || !integer(import.take(), params) ||
            !import.skip(params) || !import.done())
            return false;
    }
    for (unsigned i = 0; i < globals; ++i) {
        auto global = next();
        unsigned words;
        if (global.take() != "global" || !global.skip(1) || !integer(global.take(), words))
            return false;
        const auto kind = global.take();
        if (kind != "zero" && (kind != "init" || !global.skip(words)))
            return false;
        if (!global.done())
            return false;
    }
    auto end = next();
    return end.take() == "end" && end.done() && lines.offset == lines.text.size();
}

struct Fact {
    std::uint64_t word = 0;
    unsigned block = std::numeric_limits<unsigned>::max();
};

void discover(const Site &site, std::vector<Fact> &facts, unsigned block)
{
    const auto &inst = site.inst;
    if (inst.op == Op::const_) {
        facts[site.dst] = {inst.word, block};
        return;
    }
    if (facts[inst.use[0]].block != block)
        return;
    const auto a = facts[inst.use[0]].word;
    if (inst.op == Op::copy) {
        facts[site.dst] = {a, block};
        return;
    }
    if (inst.op < Op::copy) {
        const auto width = static_cast<unsigned>(inst.word);
        if (width == 64) {
            facts[site.dst] = {a, block};
            return;
        }
        const auto mask = (std::uint64_t{1} << width) - 1;
        const auto sign = std::uint64_t{1} << (width - 1);
        const auto extend = inst.op == Op::sext && (a & sign);
        facts[site.dst] = {(a & mask) | (extend ? ~mask : 0), block};
        return;
    }
    if (facts[inst.use[1]].block != block)
        return;
    const auto b = facts[inst.use[1]].word;
    constexpr auto sign = std::uint64_t{1} << 63;
    const unsigned shift = b & 63;
    std::uint64_t result;
    switch (inst.op) {
    case Op::add: result = a + b; break;
    case Op::sub: result = a - b; break;
    case Op::mul: result = a * b; break;
    case Op::and_: result = a & b; break;
    case Op::or_: result = a | b; break;
    case Op::xor_: result = a ^ b; break;
    case Op::shl: result = a << shift; break;
    case Op::lshr: result = a >> shift; break;
    case Op::ashr:
        result = (a >> shift) | ((a & sign) && shift ?
            (std::numeric_limits<std::uint64_t>::max() << (64 - shift)) : 0);
        break;
    case Op::eq: result = a == b; break;
    case Op::slt: result = (a ^ sign) < (b ^ sign); break;
    case Op::ult: result = a < b; break;
    default: return;
    }
    facts[site.dst] = {result, block};
}

bool matches(const Rule &rule, const Inst &inst, const std::vector<Fact> &facts, unsigned block)
{
    for (unsigned i = 0; i < arity(inst.op); ++i) {
        const auto &pattern = rule.operands[i];
        const auto operand = inst.use[i];
        if (pattern.kind == Match::same && operand != inst.use[pattern.word])
            return false;
        if (pattern.kind == Match::bits &&
            (facts[operand].block != block || facts[operand].word != pattern.word))
            return false;
    }
    return true;
}

Inst instantiate(const Rule &rule, const Inst &inst)
{
    Inst out{rule.replacement};
    if (out.op == Op::const_)
        out.word = rule.word;
    else
        for (unsigned i = 0; i < arity(out.op); ++i)
            out.use[i] = inst.use[rule.slots[i]];
    return out;
}

struct Budget {
    unsigned fuel;
    unsigned phase_fuel = max_fuel;
    unsigned attempts = max_attempts;
};

bool scan(std::vector<Function> &module, const Phase &phase, bool reject, Budget &budget,
          Diagnostics &diag, const std::string_view input)
{
    const auto &dispatch = reject ? phase.checks : phase.rewrites;
    if (reject ? !phase.has_checks : !phase.has_rewrites)
        return true;
    const auto report = [&](const Site &site, std::string_view code, const std::string &message) {
        diag.error(code, message, {std::string(input), site.line});
        return false;
    };
    std::vector<Fact> facts;
    for (auto &fn : module) {
        facts.assign(fn.value_count, {});
        for (unsigned b = 0; b < fn.blocks.size(); ++b) {
            for (auto &site : fn.blocks[b].sites) {
                for (;;) {
                    bool changed = false;
                    for (const auto &rule : dispatch[index(site.inst.op)]) {
                        if (!budget.attempts)
                            return report(site, "cephyr.qml.limit", "QML match work limit exceeded");
                        --budget.attempts;
                        if (!matches(rule, site.inst, facts, b))
                            continue;
                        if (reject)
                            return report(site, "cephyr.qml.reject", "QML phase '" + phase.name +
                                          "' rejected MachineIR by rule '" + rule.name + "'");
                        const auto next = instantiate(rule, site.inst);
                        if (next == site.inst)
                            continue;
                        if (!budget.fuel || !budget.phase_fuel)
                            return report(site, "cephyr.qml.fuel", "QML rewrite fuel exhausted in phase '" +
                                          phase.name + "' at rule '" + rule.name + "'");
                        --budget.fuel;
                        --budget.phase_fuel;
                        site.inst = next;
                        changed = true;
                        break;
                    }
                    if (!changed)
                        break;
                }
                // Only publish the final definition, never facts from an
                // intermediate candidate. Recompute all facts for each pass.
                discover(site, facts, b);
            }
        }
    }
    return true;
}

bool render(const std::vector<Function> &module, std::string &mir,
            Diagnostics &diag, std::string_view input, bool *changed)
{
    std::size_t copied = 0;
    std::string result;
    const auto too_large = [&] {
        diag.error("cephyr.qml.limit", "rewritten MachineIR text exceeds native size limit",
                   {std::string(input)});
        return false;
    };
    for (const auto &fn : module)
        for (const auto &block : fn.blocks)
            for (const auto &site : block.sites) {
                if (site.original == site.inst)
                    continue;
                if (copied == 0)
                    result.reserve(mir.size());
                result.append(mir, copied, site.offset - copied);
                result.append(op_names[index(site.inst.op)]);
                result += ' ' + std::to_string(site.dst);
                if (site.inst.op == Op::const_)
                    result += ' ' + std::to_string(site.inst.word);
                else
                    for (unsigned i = 0; i < arity(site.inst.op); ++i)
                        result += ' ' + std::to_string(site.inst.use[i]);
                copied = site.offset + site.length;
                if (result.size() > ARCHON_MIR_TEXT_MAX_BYTES)
                    return too_large();
            }
    if (copied == 0)
        return true;
    if (mir.size() - copied > ARCHON_MIR_TEXT_MAX_BYTES - result.size())
        return too_large();
    result.append(mir, copied);
    mir.swap(result);
    if (changed)
        *changed = true;
    return true;
}

} // namespace

struct Pipeline::State {
    std::vector<Phase> phases;
    std::vector<std::size_t> defaults;
    std::vector<std::size_t> active;
    std::array<unsigned, max_phases> phase_fuel{};
    std::size_t compiled_bytes = 0;
    std::size_t rules = 0;
    unsigned fuel = 0;

    auto find(std::string_view name) const
    {
        return std::find_if(phases.begin(), phases.end(),
                            [&](const Phase &phase) { return phase.name == name; });
    }
};

bool Pipeline::add_compiled(std::string_view text, Diagnostics &diag, std::string_view source)
{
    Lines lines{text};
    const auto bad = [&](std::string_view why) {
        diag.error("cephyr.qml.compiled", why, {std::string(source), lines.line});
        return false;
    };
    if (text.size() > max_compiled_bytes - (state_ ? state_->compiled_bytes : 0))
        return bad("compiled QML byte limit exceeded");
    std::string_view line;
    if (!lines.take(line))
        return bad("expected CEPHYR-QML-1 header");
    if (line.ends_with('\r'))
        line.remove_suffix(1);
    if (line != "CEPHYR-QML-1")
        return bad("expected CEPHYR-QML-1 header");
    auto next = state_ ? std::make_shared<State>(*state_) : std::make_shared<State>();
    next->compiled_bytes += text.size();
    while (lines.take(line)) {
        if (line.size() > max_rule_line)
            return bad("compiled QML line limit exceeded");
        Tokens tokens{line};
        const auto directive = tokens.take();
        if (directive.empty())
            continue;
        if (directive != "P" && directive != "R" && directive != "X")
            return bad("unknown compiled QML directive");
        const auto phase_name = tokens.take();
        if (!identifier(phase_name))
            return bad("expected a phase identifier");
        const auto found = next->find(phase_name);
        const auto phase_index = static_cast<std::size_t>(found - next->phases.begin());
        if (found == next->phases.end()) {
            if (next->phases.size() == max_phases)
                return bad("compiled QML phase limit exceeded");
            next->phases.emplace_back().name = phase_name;
        }
        if (directive == "P") {
            if (!tokens.done() || next->defaults.size() == max_phases)
                return bad("malformed or too many default QML phases");
            next->defaults.push_back(phase_index);
            continue;
        }
        if (next->rules == max_rules)
            return bad("compiled QML rule limit exceeded");
        const auto name = tokens.take();
        if (!identifier(name))
            return bad("expected a rule identifier");
        const bool reject = directive == "X";
        if (!reject && phase_name == "sema")
            return bad("the native sema phase accepts only rejection rules");
        auto &phase = next->phases[phase_index];
        Op op;
        Rule rule;
        rule.name = name;
        std::string_view error;
        if (!parse_rule(tokens, reject, op, rule, error)) {
            return bad("rule '" + rule.name + "': " + std::string(error));
        }
        auto &dispatch = reject ? phase.checks : phase.rewrites;
        dispatch[index(op)].push_back(std::move(rule));
        (reject ? phase.has_checks : phase.has_rewrites) = true;
        ++next->rules;
    }
    next->active.clear();
    next->fuel = 0;
    state_ = std::move(next);
    return true;
}

bool Pipeline::configure(const std::vector<std::string> &phases, unsigned fuel, Diagnostics &diag)
{
    return configure(phases, fuel, diag, {});
}

bool Pipeline::configure(const std::vector<std::string> &phases, unsigned fuel, Diagnostics &diag,
                         const std::map<std::string, unsigned, std::less<>> &phase_fuel)
{
    if (!fuel || fuel > max_fuel || phases.size() > max_phases || phase_fuel.size() > max_phases) {
        diag.error("cephyr.qml.config", "QML requires fuel in 1..1000000 and at most 64 phases");
        return false;
    }
    auto next = state_ ? std::make_shared<State>(*state_) : std::make_shared<State>();
    next->active.clear();
    next->fuel = fuel;
    if (phases.empty()) {
        if (!next->defaults.empty())
            next->active = next->defaults;
        else
            for (std::size_t i = 0; i < next->phases.size(); ++i)
                next->active.push_back(i);
    } else {
        for (const auto &phase : phases) {
            if (!identifier(phase)) {
                diag.error("cephyr.qml.config", "invalid native QML phase identifier");
                return false;
            }
            const auto found = next->find(phase);
            if (found == next->phases.end()) {
                diag.error("cephyr.qml.config", "unknown native QML phase '" + phase + "'");
                return false;
            }
            next->active.push_back(static_cast<std::size_t>(found - next->phases.begin()));
        }
    }
    next->phase_fuel.fill(max_fuel);
    for (const auto &[phase, cap] : phase_fuel) {
        if (!identifier(phase)) {
            diag.error("cephyr.qml.config", "invalid native QML phase fuel identifier");
            return false;
        }
        const auto found = next->find(phase);
        const auto i = static_cast<std::size_t>(found - next->phases.begin());
        if (std::find(next->active.begin(), next->active.end(), i) == next->active.end() ||
            !cap || cap > max_fuel) {
            diag.error("cephyr.qml.config", "invalid rewrite fuel cap for native QML phase '" +
                       phase + "'");
            return false;
        }
        next->phase_fuel[i] = cap;
    }
    state_ = std::move(next);
    return true;
}

bool Pipeline::rewrite(std::string &mir, Diagnostics &diag, std::string_view input) const
{
    return rewrite_impl(mir, diag, input, true, nullptr);
}

bool Pipeline::rewrite_canonical(std::string &mir, Diagnostics &diag, std::string_view input,
                                 bool *changed) const
{
    return rewrite_impl(mir, diag, input, false, changed);
}

bool Pipeline::rewrite_impl(std::string &mir, Diagnostics &diag, std::string_view input,
                            bool verify_input, bool *changed) const
{
    if (changed)
        *changed = false;
    if (!state_ || !state_->fuel) {
        diag.error("cephyr.qml.config", "native QML pipeline has not been configured");
        return false;
    }
    if (verify_input) {
        ArchonBackendDiagnostic native{};
        if (cephyr_verify_mir(mir.data(), mir.size(), &native) != ARCHON_BACKEND_OK) {
            diag.error("cephyr.qml.mir", "invalid input MachineIR: " + std::string(native.message),
                       {std::string(input)});
            return false;
        }
    } else if (mir.size() > ARCHON_MIR_TEXT_MAX_BYTES) {
        diag.error("cephyr.qml.limit", "MachineIR text exceeds native size limit",
                   {std::string(input)});
        return false;
    }
    const auto &state = *state_;
    const auto sema = state.find("sema");
    const auto check_sema = sema != state.phases.end() && sema->has_checks;
    if (!check_sema && std::none_of(state.active.begin(), state.active.end(), [&](auto i) {
            return state.phases[i].has_checks || state.phases[i].has_rewrites;
        }))
        return true;

    Lines lines{mir};
    std::vector<Function> module;
    if (!read_module(lines, module)) {
        diag.error("cephyr.qml.mir", "expected Cephyr's line-oriented MachineIR records",
                   {std::string(input), lines.line});
        return false;
    }
    Budget budget{state.fuel};
    for (auto i : state.active) {
        const auto &phase = state.phases[i];
        budget.phase_fuel = state.phase_fuel[i];
        if (!scan(module, phase, false, budget, diag, input) ||
            !scan(module, phase, true, budget, diag, input))
            return false;
    }
    if (check_sema &&
        (state.active.empty() || &state.phases[state.active.back()] != &*sema) &&
        !scan(module, *sema, true, budget, diag, input))
        return false;
    return render(module, mir, diag, input, changed);
}

} // namespace cephyr::qml
