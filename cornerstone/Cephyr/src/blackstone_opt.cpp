#include "blackstone_opt.hpp"

#include "cephyr/diagnostics.hpp"
#include "mir_verify.h"
#include "rewrite_passes.hpp"
#include <firestone-interop/match.hpp>

#include <cstdint>
#include <exception>
#include <map>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <vector>

namespace cephyr::blackstone_opt {
namespace {

using Word = std::uint64_t;
using ::blackstone::Term;
using ::blackstone::Pattern;
using ::blackstone::parse_pattern;
enum class Result : unsigned char { untested, none, fold, left, right, zero };

struct Binary {
    std::string name;
    Word (*fold)(Word, Word);
    std::vector<std::pair<Pattern, Result>> rules;

    Binary(const char *op, Word (*fn)(Word, Word),
           std::initializer_list<std::pair<const char *, Result>> identities)
        : name(op), fold(fn)
    {
        // Literal and SSA-reference terms are disjoint. A repeated pattern
        // variable matches only identical values, never two unknown producers.
        rules.emplace_back(parse_pattern("(" + name + " (k ?a) (k ?b))"), Result::fold);
        for (auto [args, result] : identities)
            rules.emplace_back(parse_pattern("(" + name + " " + args + ")"), result);
    }
};

const auto &operations()
{
    using enum Result;
    static const std::map<ArchonMIROpcode, Binary> ops = {
        {ARCHON_MIR_ADD, {"add", [](Word a, Word b) { return a + b; },
                          {{"?x (k 0)", left}, {"(k 0) ?x", right}}}},
        {ARCHON_MIR_SUB, {"sub", [](Word a, Word b) { return a - b; },
                          {{"?x (k 0)", left}, {"?x ?x", zero}}}},
        {ARCHON_MIR_MUL, {"mul", [](Word a, Word b) { return a * b; },
                          {{"?x (k 1)", left}, {"(k 1) ?x", right},
                           {"?x (k 0)", zero}, {"(k 0) ?x", zero}}}},
        {ARCHON_MIR_AND, {"and", [](Word a, Word b) { return a & b; },
                          {{"?x ?x", left}, {"?x (k 0)", zero}, {"(k 0) ?x", zero}}}},
        {ARCHON_MIR_OR,  {"or", [](Word a, Word b) { return a | b; },
                          {{"?x ?x", left}, {"?x (k 0)", left}, {"(k 0) ?x", right}}}},
        {ARCHON_MIR_XOR, {"xor", [](Word a, Word b) { return a ^ b; },
                          {{"?x ?x", zero}, {"?x (k 0)", left}, {"(k 0) ?x", right}}}},
    };
    return ops;
}

struct Replacement {
    bool literal;
    Word value; // either the bit pattern or an existing i64 SSA ID
};

// These patterns observe literal vs reference, the exact literals zero/one,
// and equality via a repeated variable. No pattern inspects an SSA ID or any
// other literal. Memoize the *rule*, never a binding or a folded result.
struct Matcher {
    std::uint64_t passes;
    Result decisions[ARCHON_MIR_XOR + 1][4][4][2]{};

    static unsigned kind(const Word *k)
    {
        return !k ? 0 : *k == 0 ? 1 : *k == 1 ? 2 : 3;
    }

    static Term operand(ArchonValue id, const Word *literal)
    {
        if (literal)
            return {"k", {Term(std::to_string(*literal))}};
        return {"v", {Term(std::to_string(id))}};
    }

    Result select(const Binary &binary, const ArchonMIRInst &i, const Word *a, const Word *b)
    {
        const bool same = i.a == i.b || (a && b && *a == *b);
        auto &result = decisions[i.op][kind(a)][kind(b)][same];
        if (result == Result::untested) {
            const Term expr{binary.name, {operand(i.a, a), operand(i.b, b)}};
            result = Result::none;
            for (const auto &[pattern, action] : binary.rules) {
                const auto pass = action == Result::fold ? BlackstonePass::constant_fold :
                                                          BlackstonePass::identities;
                if (pass_enabled(passes, pass) && ::blackstone::match(pattern, expr)) {
                    result = action;
                    break;
                }
            }
        }
        return result;
    }
};

// The input has passed Archon's SSA, dominance and type verifier. Keep facts
// block-local; block parameters and all unmodeled definitions are opaque.
struct Block {
    const ArchonMIRFunction &function;
    Matcher &matcher;
    std::unordered_map<ArchonValue, Word> literals;

    bool i64(ArchonValue id) const
    {
        return id < function.value_count && function.types[id] == ARCHON_MIR_I64;
    }

    const Word *literal(ArchonValue id) const
    {
        auto it = literals.find(id);
        return it != literals.end() ? &it->second : nullptr;
    }

    std::optional<Replacement> visit(const ArchonMIRInst &i)
    {
        if (!i64(i.dst))
            return {};
        if (i.op == ARCHON_MIR_CONST) {
            literals.emplace(i.dst, i.imm);
            return {};
        }
        if (!i64(i.a))
            return {};
        if (i.op == ARCHON_MIR_COPY || i.op == ARCHON_MIR_SEXT ||
            i.op == ARCHON_MIR_ZEXT || i.op == ARCHON_MIR_TRUNC) {
            if (auto src = literals.find(i.a); src != literals.end()) {
                Word bits = src->second;
                if (i.op != ARCHON_MIR_COPY && i.imm < 64) {
                    // The verifier enforces width in [1,64].
                    const Word mask = (Word{1} << i.imm) - 1;
                    bits &= mask;
                    if (i.op == ARCHON_MIR_SEXT && bits > (mask >> 1))
                        bits |= ~mask;
                }
                literals.emplace(i.dst, bits);
            }
            return {};
        }
        auto op = operations().find(i.op);
        if (op == operations().end() || !i64(i.b))
            return {};
        const Word *a = literal(i.a), *b = literal(i.b);
        // Every pattern needs either two literals, a zero/one, or identical
        // references. Avoid allocating terms for inert and sparse regions.
        if (i.a != i.b && !(a && b) && !(a && *a <= 1) && !(b && *b <= 1))
            return {};
        const auto &binary = op->second;
        auto result = matcher.select(binary, i, a, b);
        if (result == Result::none)
            return {};
        if (result == Result::left || result == Result::right)
            return Replacement{false, result == Result::left ? i.a : i.b};
        const Word bits = result == Result::fold ? binary.fold(*a, *b) : 0;
        literals.emplace(i.dst, bits);
        // Reuse an existing dominating word rather than materializing the
        // same bits again. A copy may be coalesced by the native allocator.
        if (a && *a == bits)
            return Replacement{false, i.a};
        if (b && *b == bits)
            return Replacement{false, i.b};
        return Replacement{true, bits};
    }
};

struct Rewriter {
    const std::string &source;
    std::uint64_t passes;
    std::string output;
    std::size_t cursor = 0;
    std::exception_ptr error;

    // Walk the verified token stream, using the native parser's instruction
    // spans. Symbols, immediates, local indices, edge targets and counts are
    // never interpreted as SSA references.
    std::size_t pos = 0;
    static constexpr auto ws = " \t\r\n";

    void edit(std::size_t begin, std::size_t end, std::string_view text)
    {
        output.append(source, cursor, begin - cursor);
        output.append(text);
        cursor = end;
    }

    void token(std::optional<std::size_t> replacement = {}, bool drop = false)
    {
        auto begin = source.find_first_not_of(ws, pos);
        auto end = source.find_first_of(ws, begin);
        if (end == std::string::npos)
            end = source.size();
        if (drop)
            edit(pos, end, {});
        else if (replacement)
            edit(begin, end, std::to_string(*replacement));
        pos = end;
    }

    void run(const ArchonMIRModule &m, const size_t *starts, const size_t *ends)
    {
        Matcher matcher{passes};
        const bool coalesce = pass_enabled(passes, BlackstonePass::coalesce_copies);
        const bool prune = pass_enabled(passes, BlackstonePass::remove_dead_constants);
        for (int n = 0; n < 6; ++n)
            token(); // archon-mir version module functions imports globals
        std::size_t span = 0;
        for (std::size_t f = 0; f < m.function_count; ++f) {
            const auto &fn = m.functions[f];
            std::vector<ArchonValue> alias(fn.value_count), id(fn.value_count);
            std::iota(alias.begin(), alias.end(), ArchonValue{0});
            std::vector<std::optional<Replacement>> rewrites(fn.value_count);
            std::vector<std::size_t> removed(fn.block_count);
            auto root = [&](ArchonValue v) {
                if (v != ARCHON_MIR_NONE)
                    while (v != alias[v]) {
                        alias[v] = alias[alias[v]];
                        v = alias[v];
                    }
                return v;
            };
            for (std::size_t b = 0; b < fn.block_count; ++b) {
                Block block{fn, matcher, {}};
                const auto &bb = fn.blocks[b];
                for (std::size_t n = 0; n < bb.inst_count; ++n) {
                    auto i = bb.insts[n];
                    if (!block.i64(i.dst))
                        continue;
                    i.a = root(i.a);
                    i.b = root(i.b);
                    auto r = block.visit(i);
                    if (coalesce && (i.op == ARCHON_MIR_COPY || (i.imm == 64 &&
                        (i.op == ARCHON_MIR_ZEXT || i.op == ARCHON_MIR_SEXT ||
                         i.op == ARCHON_MIR_TRUNC))))
                        r = Replacement{false, i.a};
                    if (r && !r->literal) {
                        // Every link is a verified, dominating i64 SSA def.
                        alias[i.dst] = static_cast<ArchonValue>(r->value);
                        ++removed[b];
                    }
                    rewrites[i.dst] = r;
                }
            }
            // Discard unused word literals (including newly folded ones).
            // MIR doesn't distinguish volatile loads; even a dead load must
            // remain. Count uses from all blocks, both edges and call args.
            std::vector<bool> dead(fn.value_count);
            if (prune) {
                std::vector<bool> used(fn.value_count);
                auto use = [&](ArchonValue v) {
                    if (v != ARCHON_MIR_NONE)
                        used[root(v)] = true;
                };
                for (std::size_t b = 0; b < fn.block_count; ++b) {
                    const auto &bb = fn.blocks[b];
                    for (std::size_t n = 0; n < bb.inst_count; ++n) {
                        const auto &i = bb.insts[n];
                        if (i.dst != ARCHON_MIR_NONE && rewrites[i.dst])
                            continue;
                        use(i.a); use(i.b);
                        for (std::size_t j = 0; j < i.arg_count; ++j) use(i.args[j]);
                    }
                    use(bb.term.value);
                    for (auto e : {bb.term.yes, bb.term.no})
                        for (std::size_t j = 0; j < e.arg_count; ++j) use(e.args[j]);
                }
                for (std::size_t b = 0; b < fn.block_count; ++b) {
                    const auto &bb = fn.blocks[b];
                    for (std::size_t n = 0; n < bb.inst_count; ++n) {
                        const auto &i = bb.insts[n];
                        if (i.dst != ARCHON_MIR_NONE && !used[i.dst] &&
                            (i.op == ARCHON_MIR_CONST || (rewrites[i.dst] && rewrites[i.dst]->literal))) {
                            dead[i.dst] = true;
                            ++removed[b];
                        }
                    }
                }
            }
            std::size_t kept = 0;
            for (ArchonValue v = 0; v < fn.value_count; ++v)
                if (alias[v] == v && !dead[v])
                    id[v] = kept++;
            const bool compact = kept != fn.value_count;
            auto rename = [&](ArchonValue v) {
                return id[root(v)];
            };
            auto value = [&](ArchonValue v) {
                if (compact && v != ARCHON_MIR_NONE && rename(v) != v)
                    token(rename(v));
                else
                    token();
            };
            token(); token(); token(); token(); // function name result arity
            token(compact ? std::optional(kept) : std::nullopt);
            token(); token(); token(); // block/local counts, types
            for (ArchonValue v = 0; v < fn.value_count; ++v)
                token({}, v != alias[v] || dead[v]);
            for (std::size_t n = 0; n < fn.local_count; ++n) {
                token(); token(); token();
            }
            for (std::size_t b = 0; b < fn.block_count; ++b) {
                const auto &bb = fn.blocks[b];
                token(); token(); // block, parameter count
                token(removed[b] ? std::optional(bb.inst_count - removed[b]) : std::nullopt);
                token(); // params
                for (std::size_t n = 0; n < bb.param_count; ++n)
                    value(bb.params[n]);
                for (std::size_t n = 0; n < bb.inst_count; ++n, ++span) {
                    const auto &i = bb.insts[n];
                    if (i.dst != ARCHON_MIR_NONE && dead[i.dst]) {
                        edit(pos, ends[span], {});
                    } else if (i.dst != ARCHON_MIR_NONE && rewrites[i.dst]) {
                        auto r = *rewrites[i.dst];
                        if (!r.literal)
                            edit(pos, ends[span], {});
                        else
                            edit(starts[span], ends[span], "const " +
                                 std::to_string(id[i.dst]) + " " + std::to_string(r.value));
                    } else if (compact) {
                        token(); // opcode
                        const bool call = i.op == ARCHON_MIR_CALL || i.op == ARCHON_MIR_CALL_INDIRECT;
                        if (i.dst != ARCHON_MIR_NONE || call) value(i.dst);
                        if (i.a != ARCHON_MIR_NONE) value(i.a);
                        if (i.b != ARCHON_MIR_NONE) value(i.b);
                        if (call) {
                            token(); token(); // signature symbol, argument count
                            for (std::size_t j = 0; j < i.arg_count; ++j)
                                value(i.args[j]);
                        }
                    }
                    pos = ends[span];
                }
                const auto &t = bb.term;
                token();
                if (t.kind == ARCHON_MIR_RETURN || t.kind == ARCHON_MIR_CBR)
                    value(t.value);
                auto edge = [&](const ArchonMIREdge &e) {
                    token(); token(); // target, argument count
                    for (std::size_t j = 0; j < e.arg_count; ++j)
                        value(e.args[j]);
                };
                if (t.kind == ARCHON_MIR_BR || t.kind == ARCHON_MIR_CBR) {
                    edge(t.yes);
                    if (t.kind == ARCHON_MIR_CBR)
                        edge(t.no);
                }
            }
        }
        if (cursor)
            output.append(source, cursor);
    }
};

void visit(void *user, const ArchonMIRModule *module, const size_t *starts,
           const size_t *ends) noexcept
{
    auto &rewriter = *static_cast<Rewriter *>(user);
    try {
        rewriter.run(*module, starts, ends);
    } catch (...) {
        rewriter.error = std::current_exception();
    }
}

} // namespace

bool rewrite(std::string &mir, Diagnostics &diag, std::string_view input, bool &changed,
             std::uint64_t passes)
{
    changed = false;
    ArchonBackendDiagnostic native{};
    Rewriter rewriter{mir, passes, {}, 0, {}};
    if (cephyr_visit_mir(mir.data(), mir.size(), visit, &rewriter, &native) != ARCHON_BACKEND_OK) {
        diag.error("cephyr.blackstone.input", native.message, {std::string(input)});
        return false;
    }
    if (rewriter.error) {
        try {
            std::rethrow_exception(rewriter.error);
        } catch (const std::exception &ex) {
            diag.error("cephyr.blackstone.rewrite", ex.what(), {std::string(input)});
        } catch (...) {
            diag.error("cephyr.blackstone.rewrite", "unexpected matcher failure", {std::string(input)});
        }
        return false;
    }
    // A large folded literal can take more bytes than the original expression.
    // Do not reject a valid module merely because its optimized form is larger.
    if (!rewriter.cursor || rewriter.output.size() > ARCHON_MIR_TEXT_MAX_BYTES)
        return true;
    if (cephyr_verify_mir(rewriter.output.data(), rewriter.output.size(), &native) != ARCHON_BACKEND_OK) {
        diag.error("cephyr.blackstone.output", native.message, {std::string(input)});
        return false;
    }
    mir.swap(rewriter.output);
    changed = true;
    return true;
}
} // namespace cephyr::blackstone_opt
