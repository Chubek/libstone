#ifndef CEPHYR_FACT_IR_HPP
#define CEPHYR_FACT_IR_HPP

#include "src/mir_verify.h"

#include <cephyr/diagnostics.hpp>
#include <cephyr/target.hpp>
#include <novostone/Datalog/Datalog.hpp>
#include <SatieCDCL.hpp>
#include <SatieTheoryUtils.hpp>
#include <weighstone/support/KnownBitMasks.hpp>

#include <algorithm>
#include <exception>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace cephyr::factir {
namespace dl = novostone::datalog;

inline constexpr std::string_view schema = "cephyr-factir-v2";
inline constexpr dl::Limits relation_limits{150'000, 1'500'000};

using Bits = weighstone::support::KnownBitMasks<std::uint64_t>;
struct KnownBitsRow {
    std::size_t function;
    ArchonValue value;
    Bits bits;
};

struct Result {
    // Sorted by (function ordinal, SSA value). IDs are unique within a function.
    std::vector<std::pair<std::size_t, ArchonValue>> preferred;
    std::vector<KnownBitsRow> known_bits;
    std::vector<std::tuple<ArchonBackendFactKind, std::size_t, std::size_t, std::uint32_t>> coordinates;
    std::string source_mir, triple, architecture, abi;
    std::filesystem::path specification;
    std::string smtlib, facts;
    std::size_t rewrites = 0;
    bool budget_exceeded = false;

    static int prefer(const void *user, std::size_t function, std::size_t, ArchonValue dst) noexcept
    {
        const auto &rows = static_cast<const Result *>(user)->preferred;
        return std::binary_search(rows.begin(), rows.end(), std::pair{function, dst});
    }

    static int query_known_bits(const void *user, std::size_t function, ArchonValue value,
                                std::uint64_t *zero, std::uint64_t *one) noexcept
    {
        const auto &rows = static_cast<const Result *>(user)->known_bits;
        const auto i = std::lower_bound(rows.begin(), rows.end(), std::pair{function, value},
            [](const KnownBitsRow &row, auto id) { return std::pair{row.function, row.value} < id; });
        if (i == rows.end() || i->function != function || i->value != value)
            return 0;
        *zero = i->bits.zero;
        *one = i->bits.one;
        return 1;
    }

    bool matches(std::string_view mir, const Target &target) const
    {
        return !budget_exceeded && source_mir == mir && triple == target.triple &&
            architecture == target.architecture && abi == target.abi &&
            specification == target.specification;
    }

    bool contains(ArchonBackendFactKind kind, std::size_t function, std::size_t block,
                  std::uint32_t key) const noexcept
    {
        switch (kind) {
        case ARCHON_BACKEND_FACT_OBSERVABLE_INPUT:
            return prefer(this, function, block, key);
        case ARCHON_BACKEND_FACT_CONTROL_EDGE:
        case ARCHON_BACKEND_FACT_RETURN_VALUE:
            return std::binary_search(coordinates.begin(), coordinates.end(),
                                      std::tuple{kind, function, block, key});
        }
        return false;
    }
};

inline satie::SolveResult solve_obligation(satie::theory::Encoder &enc, std::uint64_t conflict_budget)
{
    satie::CNF cnf;
    for (auto &clause : enc.take_clauses())
        cnf.add_clause(std::move(clause));
    satie::CDCLSolver solver(std::move(cnf));
    solver.set_conflict_budget(conflict_budget);
    return solver.solve();
}

// The proof checks for a counterexample to wrap_add64(x, k) == copy64(x).
// Carry-out is explicitly dropped.
inline satie::SolveResult prove_add_constant(std::uint64_t k, std::uint64_t conflict_budget = 256)
{
    satie::theory::Encoder enc(1);
    std::vector<satie::Lit> x(64), constant(64), difference(64);
    for (std::size_t bit = 0; bit < 64; ++bit) {
        x[bit] = enc.fresh_lit();
        constant[bit] = ((k >> bit) & 1u) ? enc.const_true() : enc.const_false();
    }
    auto sum = enc.ripple_add(x, constant, enc.const_false());
    for (std::size_t bit = 0; bit < 64; ++bit)
        difference[bit] = enc.lxor2(sum[bit], x[bit]);
    enc.add_clause(std::move(difference));
    return solve_obligation(enc, conflict_budget);
}

// eq(eq(x,0),0) yields the i64 zero-extension of (x != 0).
inline satie::SolveResult prove_boolize(std::uint64_t known_zero)
{
    satie::theory::Encoder enc(1);
    auto nonzero = enc.const_false();
    std::vector<satie::Lit> x(64);
    for (unsigned bit = 0; bit < 64; ++bit) {
        x[bit] = (known_zero >> bit) & 1 ? enc.const_false() : enc.fresh_lit();
        nonzero = enc.lor2(nonzero, x[bit]);
    }
    x[0] = enc.lxor2(nonzero, x[0]);
    enc.add_clause(std::move(x)); // At least one result bit differs.
    return solve_obligation(enc, 256);
}

inline dl::Program rules()
{
    using enum dl::Type;
    const auto f = dl::Term::var("Function"), x = dl::Term::var("Input"),
               y = dl::Term::var("Middle"), z = dl::Term::var("Output");
    dl::Program program;
    program.declare("flow", {Integer, Integer, Integer});
    program.declare("sink", {Integer, Integer});
    program.declare("control_edge", {Integer, Integer, Integer});
    program.declare("return_value", {Integer, Integer, Integer});
    program.declare("direct_call", {Integer, Symbol});
    program.declare("function", {Integer, Symbol});
    program.declare("dependency", {Integer, Integer, Integer}, true);
    program.declare("observable_input", {Integer, Integer}, true);
    program.add({{"dependency", {f, x, z}}, {{"flow", {f, x, z}}}});
    program.add({{"dependency", {f, x, z}}, {{"flow", {f, x, y}}, {"dependency", {f, y, z}}}});
    program.add({{"observable_input", {f, x}}, {{"sink", {f, x}}}});
    program.add({{"observable_input", {f, x}}, {{"flow", {f, x, y}}, {"observable_input", {f, y}}}});
    return program;
}

inline bool pure(ArchonMIROpcode op)
{
    switch (op) {
    case ARCHON_MIR_CONST: case ARCHON_MIR_COPY:
    case ARCHON_MIR_ADD: case ARCHON_MIR_SUB: case ARCHON_MIR_MUL:
    case ARCHON_MIR_AND: case ARCHON_MIR_OR: case ARCHON_MIR_XOR:
    case ARCHON_MIR_SHL: case ARCHON_MIR_LSHR: case ARCHON_MIR_ASHR:
    case ARCHON_MIR_EQ: case ARCHON_MIR_SLT: case ARCHON_MIR_ULT:
    case ARCHON_MIR_TRUNC: case ARCHON_MIR_ZEXT: case ARCHON_MIR_SEXT:
        return true;
    default:
        return false; // Includes checked arithmetic, calls, memory, and all unmodeled ops.
    }
}

inline Bits infer_bits(const ArchonMIRInst &ins, Bits a, Bits b)
{
    switch (ins.op) {
    case ARCHON_MIR_CONST: return {~ins.imm, ins.imm};
    case ARCHON_MIR_COPY: return a;
    case ARCHON_MIR_AND: return a.bit_and(b);
    case ARCHON_MIR_OR: return a.bit_or(b);
    case ARCHON_MIR_XOR: return a.bit_xor(b);
    case ARCHON_MIR_LOAD_U8: return {~UINT64_C(0xff), 0};
    case ARCHON_MIR_LOAD_U16: return {~UINT64_C(0xffff), 0};
    case ARCHON_MIR_LOAD_U32: return {~UINT64_C(0xffffffff), 0};
    case ARCHON_MIR_EQ: case ARCHON_MIR_SLT: case ARCHON_MIR_ULT:
        return {~UINT64_C(1), 0};
    case ARCHON_MIR_SHL: case ARCHON_MIR_LSHR:
        if (b.all_known(UINT64_MAX) && b.one < 64) {
            if (b.one == 0)
                return a;
            if (ins.op == ARCHON_MIR_SHL)
                return {(a.zero << b.one) | ((UINT64_C(1) << b.one) - 1), a.one << b.one};
            return {(a.zero >> b.one) | (UINT64_MAX << (64 - b.one)), a.one >> b.one};
        }
        return {};
    case ARCHON_MIR_TRUNC: case ARCHON_MIR_ZEXT: case ARCHON_MIR_SEXT: {
        if (ins.imm == 64)
            return a;
        const auto sign = UINT64_C(1) << (ins.imm - 1); // Verified width 1..63.
        const auto upper = ~(sign * 2 - 1);
        return {(a.zero & ~upper) | ((ins.op != ARCHON_MIR_SEXT || (a.zero & sign)) ? upper : 0),
                (a.one & ~upper) | ((ins.op == ARCHON_MIR_SEXT && (a.one & sign)) ? upper : 0)};
    }
    default:
        return {};
    }
}

// Each known bit is true on every execution reaching its SSA definition.
// Initialize to unknown (including every loop backedge) and only refine by
// sound local transfers and intersection across ALL incoming edges. Each
// partial solution is sound, so bounding sweeps only loses precision.
inline std::vector<Bits> function_known_bits(const ArchonMIRFunction &fn)
{
    std::vector<Bits> bits(fn.value_count);
    std::vector<std::vector<const ArchonMIREdge *>> incoming(fn.block_count);
    for (std::size_t b = 0; b < fn.block_count; ++b) {
        const auto &term = fn.blocks[b].term;
        if (term.kind == ARCHON_MIR_BR || term.kind == ARCHON_MIR_CBR)
            incoming[term.yes.target].push_back(&term.yes);
        if (term.kind == ARCHON_MIR_CBR)
            incoming[term.no.target].push_back(&term.no);
    }
    for (unsigned sweep = 0; sweep < 8; ++sweep) {
        bool changed = false;
        const auto set = [&](ArchonValue v, Bits next) {
            if (bits[v].zero != next.zero || bits[v].one != next.one) {
                bits[v] = next;
                changed = true;
            }
        };
        const auto get = [&](ArchonValue v) { return v == ARCHON_MIR_NONE ? Bits{} : bits[v]; };
        for (std::size_t b = 0; b < fn.block_count; ++b) {
            const auto &block = fn.blocks[b];
            for (std::size_t i = 0; i < block.param_count; ++i) {
                const auto &pred = incoming[b];
                if (fn.types[block.params[i]] != ARCHON_MIR_I64 || pred.empty())
                    continue;
                auto joined = get(pred.front()->args[i]);
                for (std::size_t e = 1; e < pred.size(); ++e)
                    joined = joined.join(get(pred[e]->args[i]));
                set(block.params[i], joined);
            }
            for (std::size_t i = 0; i < block.inst_count; ++i) {
                const auto &ins = block.insts[i];
                if (ins.dst != ARCHON_MIR_NONE && fn.types[ins.dst] == ARCHON_MIR_I64)
                    set(ins.dst, infer_bits(ins, get(ins.a), get(ins.b)));
            }
        }
        if (!changed)
            break;
    }
    return bits;
}

enum class Identity { add_zero, boolize };
struct Edit {
    std::size_t begin, end;
    ArchonValue dst, source;
    Identity identity;
};

struct Scan {
    bool find_rewrites;
    std::vector<Edit> edits;
    dl::Facts facts;
    std::exception_ptr error;
    std::size_t row_count = 0;
    bool over_budget = false;
    std::vector<KnownBitsRow> known_bits;

    explicit Scan(bool rewrites) : find_rewrites(rewrites) {}

    void insert(const char *relation, dl::Tuple row)
    {
        if (!over_budget && facts[relation].insert(std::move(row)).second &&
            ++row_count > relation_limits.facts) {
            facts.clear();
            over_budget = true;
        }
    }

    void collect(const ArchonMIRModule &module, const std::size_t *starts, const std::size_t *ends)
    {
        std::size_t span = 0;
        for (std::size_t f = 0; f < module.function_count; ++f) {
            const auto &fn = module.functions[f];
            const auto fid = static_cast<std::int64_t>(f);
            insert("function", {fid, std::string(fn.name)});
            std::vector<Bits> bits;
            if (!over_budget) {
                bits = function_known_bits(fn);
                for (ArchonValue v = 0; v < bits.size(); ++v)
                    if (bits[v].zero || bits[v].one)
                        known_bits.push_back({f, v, bits[v]});
            }
            std::vector<std::size_t> zero_in_block(find_rewrites ? fn.value_count : 0);
            std::vector<ArchonValue> negated(find_rewrites ? bits.size() : 0, ARCHON_MIR_NONE);
            for (std::size_t b = 0; b < fn.block_count; ++b) {
                const auto &block = fn.blocks[b];
                const auto sink = [&](ArchonValue v) {
                    if (v != ARCHON_MIR_NONE)
                        insert("sink", {fid, std::int64_t{v}});
                };
                const auto flow = [&](ArchonValue from, ArchonValue to) {
                    insert("flow", {fid, std::int64_t{from}, std::int64_t{to}});
                };
                const auto edge = [&](const ArchonMIREdge &e) {
                    insert("control_edge", {fid, static_cast<std::int64_t>(b), std::int64_t{e.target}});
                    for (std::size_t k = 0; k < e.arg_count; ++k)
                        flow(e.args[k], fn.blocks[e.target].params[k]);
                };
                for (std::size_t i = 0; i < block.inst_count; ++i, ++span) {
                    const auto &ins = block.insts[i];
                    if (find_rewrites) {
                        const auto known_zero = [&](ArchonValue v) {
                            return v < zero_in_block.size() && zero_in_block[v] == b + 1;
                        };
                        // A known zero is a literal or the result of a total,
                        // zero-preserving i64 operation on such a value.
                        // The shared verifier validates conversion widths and types.
                        if ((ins.op == ARCHON_MIR_CONST && ins.imm == 0) ||
                            ((ins.op == ARCHON_MIR_COPY || ins.op == ARCHON_MIR_TRUNC ||
                              ins.op == ARCHON_MIR_ZEXT || ins.op == ARCHON_MIR_SEXT) &&
                             known_zero(ins.a))) {
                            zero_in_block[ins.dst] = b + 1;
                        } else if (ins.op == ARCHON_MIR_ADD && fn.types[ins.dst] == ARCHON_MIR_I64 &&
                                   fn.types[ins.a] == ARCHON_MIR_I64 && fn.types[ins.b] == ARCHON_MIR_I64 &&
                                   (known_zero(ins.a) || known_zero(ins.b))) {
                            edits.push_back({starts[span], ends[span], ins.dst,
                                             known_zero(ins.b) ? ins.a : ins.b, Identity::add_zero});
                        } else if (ins.op == ARCHON_MIR_EQ && !bits.empty() &&
                                   fn.types[ins.a] == ARCHON_MIR_I64 && fn.types[ins.b] == ARCHON_MIR_I64 &&
                                   (known_zero(ins.a) || known_zero(ins.b))) {
                            const auto v = known_zero(ins.b) ? ins.a : ins.b;
                            if (negated[v] != ARCHON_MIR_NONE)
                                edits.push_back({starts[span], ends[span], ins.dst, negated[v], Identity::boolize});
                            // Remember !v only when v itself is a canonical boolean.
                            if ((bits[v].zero | UINT64_C(1)) == UINT64_MAX)
                                negated[ins.dst] = v;
                        }
                    }
                    if (over_budget)
                        continue;
                    const auto operand = [&](ArchonValue v) {
                        if (v == ARCHON_MIR_NONE)
                            return;
                        if (ins.dst != ARCHON_MIR_NONE)
                            flow(v, ins.dst);
                        if (!pure(ins.op))
                            sink(v);
                    };
                    operand(ins.a);
                    operand(ins.b);
                    for (std::size_t a = 0; a < ins.arg_count; ++a)
                        operand(ins.args[a]);
                    if (ins.op == ARCHON_MIR_CALL)
                        insert("direct_call", {fid, std::string(ins.symbol)});
                }
                if (!over_budget) {
                    sink(block.term.value);
                    if (block.term.kind == ARCHON_MIR_RETURN && block.term.value != ARCHON_MIR_NONE)
                        insert("return_value", {fid, static_cast<std::int64_t>(b),
                                                std::int64_t{block.term.value}});
                    if (block.term.kind == ARCHON_MIR_BR || block.term.kind == ARCHON_MIR_CBR)
                        edge(block.term.yes);
                    if (block.term.kind == ARCHON_MIR_CBR)
                        edge(block.term.no);
                }
            }
        }
    }

    static void visit(void *user, const ArchonMIRModule *module, const std::size_t *starts,
                      const std::size_t *ends) noexcept
    {
        auto &self = *static_cast<Scan *>(user);
        try {
            self.collect(*module, starts, ends);
        } catch (...) {
            self.error = std::current_exception();
        }
    }
};

inline bool scan(std::string_view input, Scan &scan, Diagnostics &diag, const std::string &name)
{
    ArchonBackendDiagnostic diagnostic{};
    if (cephyr_visit_mir(input.data(), input.size(), Scan::visit, &scan, &diagnostic) != ARCHON_BACKEND_OK) {
        diag.error("cephyr.factir.mir", diagnostic.message, Location{name});
        return false;
    }
    if (scan.error)
        std::rethrow_exception(scan.error);
    return true;
}

inline std::optional<Result> run(std::string &mir, const Target &target, const std::string &name,
                                 Diagnostics &diag)
{
    Result result;
    Scan candidates{true};
    if (!scan(mir, candidates, diag, name))
        return std::nullopt;
    result.smtlib = "(set-logic QF_BV)\n";
    std::string transformed;
    if (!candidates.edits.empty()) {
        result.smtlib += "(declare-fun x () (_ BitVec 64))\n";
        const auto proved = [&](Identity id) {
            const auto count = std::count_if(candidates.edits.begin(), candidates.edits.end(),
                                             [&](const Edit &e) { return e.identity == id; });
            if (!count)
                return false;
            result.smtlib += "(push 1)\n";
            if (id == Identity::add_zero)
                result.smtlib += "; add x,0 -> copy x (verified zero operand)\n"
                                 "(assert (distinct (bvadd x (_ bv0 64)) x))\n";
            else
                result.smtlib += "; eq(eq(x,0),0) -> copy x (known-zero high bits)\n"
                                 "(assert (= ((_ extract 63 1) x) (_ bv0 63)))\n"
                                 "(assert (distinct (ite (= x (_ bv0 64)) (_ bv0 64) (_ bv1 64)) x))\n";
            const auto proof = id == Identity::add_zero ? prove_add_constant(0) : prove_boolize(~UINT64_C(1));
            result.smtlib += "(check-sat)\n(pop 1)\n";
            if (proof.unsatisfiable())
                result.smtlib += "; Satie: unsat; applied to " + std::to_string(count) + " instruction(s)\n";
            else
                result.smtlib += proof.satisfiable() ? "; Satie: sat; not applied\n" :
                                                       "; Satie: unknown; not applied\n";
            return proof.unsatisfiable();
        };
        const bool add = proved(Identity::add_zero), boolean = proved(Identity::boolize);
        std::size_t previous = 0;
        for (const auto &edit : candidates.edits) {
            if (!(edit.identity == Identity::add_zero ? add : boolean))
                continue;
            transformed.append(mir, previous, edit.begin - previous);
            transformed += "copy " + std::to_string(edit.dst) + " " + std::to_string(edit.source);
            previous = edit.end;
            ++result.rewrites;
        }
        if (result.rewrites)
            transformed.append(mir, previous);
    } else {
        result.smtlib += "; Cephyr FactIR: no applicable candidate\n";
    }
    result.smtlib += "(exit)\n";
    Scan post_rewrite{false};
    const auto &verified_input = result.rewrites ? transformed : mir;
    if (result.rewrites) {
        candidates.facts.clear();
        candidates.known_bits.clear();
        if (!scan(verified_input, post_rewrite, diag, name))
            return std::nullopt;
    }
    auto &relations = result.rewrites ? post_rewrite : candidates;

    const auto program = rules();
    // Stage all facts in this TU-local result. Nothing escapes before the
    // verified MIR and its matching backend coordinates are ready together.
    std::ostringstream dump;
    dump.imbue(std::locale::classic());
    dump << schema << '\n';
    try {
        if (relations.over_budget)
            throw dl::BudgetExceeded("FactIR input relation budget exceeded");
        for (const auto &relation : {"observable_input", "dependency"}) {
            auto rows = program.evaluate(relations.facts, relation, relation_limits).rows;
            if (std::string_view(relation) == "observable_input")
                for (const auto &row : rows)
                    result.preferred.emplace_back(std::get<std::int64_t>(row[0]), std::get<std::int64_t>(row[1]));
            dump << '[' << relation << "]\n" << dl::dump(rows);
        }
        for (const auto &[relation, rows] : relations.facts) {
            const unsigned kind = relation == "control_edge" ? ARCHON_BACKEND_FACT_CONTROL_EDGE :
                relation == "return_value" ? ARCHON_BACKEND_FACT_RETURN_VALUE : 0;
            if (kind)
                for (const auto &row : rows)
                    result.coordinates.emplace_back(static_cast<ArchonBackendFactKind>(kind),
                        std::get<std::int64_t>(row[0]), std::get<std::int64_t>(row[1]),
                        std::get<std::int64_t>(row[2]));
            dump << '[' << relation << "]\n" << dl::dump(rows);
        }
        dump << "[known_bits_i64]\n";
        for (const auto &[f, v, masks] : relations.known_bits) {
            dump << '(' << f << ", " << v << ", " << masks.zero << ", " << masks.one << ")\n";
        }
        result.known_bits = std::move(relations.known_bits);
        std::sort(result.coordinates.begin(), result.coordinates.end());
        result.facts = std::move(dump).str();
    } catch (const dl::BudgetExceeded &) {
        result.budget_exceeded = true;
        result.facts = std::string(schema) + "\nDatalog budget exceeded; no facts exported or consumed\n";
        result.preferred.clear();
        result.coordinates.clear();
    }
    result.source_mir = verified_input;
    result.triple = target.triple;
    result.architecture = target.architecture;
    result.abi = target.abi;
    result.specification = target.specification;
    if (result.rewrites)
        mir.swap(transformed);
    return result;
}
} // namespace cephyr::factir

#endif
