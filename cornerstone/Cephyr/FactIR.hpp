#ifndef CEPHYR_FACT_IR_HPP
#define CEPHYR_FACT_IR_HPP

#include "src/driver_io.hpp"
#include "src/mir_verify.h"

#include <cephyr/diagnostics.hpp>
#include <cephyr/target.hpp>
#include <novostone/Datalog/Datalog.hpp>
#include <SatieCDCL.hpp>
#include <SatieTheoryUtils.hpp>
#include <lmdb++.h>
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

// The proof checks for a counterexample to wrap_add64(x, k) == copy64(x).
// Both are pure, total MIR i64 operations. Carry-out is explicitly dropped.
// The rewrite neither removes the zero's defining instruction nor replaces,
// duplicates, reorders, or speculates any of the input computations.
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
    satie::CNF cnf;
    for (auto &clause : enc.take_clauses())
        cnf.add_clause(std::move(clause));
    satie::CDCLSolver solver(std::move(cnf));
    solver.set_conflict_budget(conflict_budget);
    return solver.solve();
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

struct Edit {
    std::size_t begin, end;
    ArchonValue dst, source;
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
            if (!over_budget) {
                const auto bits = function_known_bits(fn);
                for (ArchonValue v = 0; v < bits.size(); ++v)
                    if (bits[v].zero || bits[v].one)
                        known_bits.push_back({f, v, bits[v]});
            }
            std::vector<std::size_t> zero_in_block(find_rewrites ? fn.value_count : 0);
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
                                             known_zero(ins.b) ? ins.a : ins.b});
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

inline void persist(Result &result, const Target &target, std::string_view name,
                    std::string_view mir, const std::string &signature, const std::string &hints,
                    const std::string &bits, const std::string &coordinates,
                    const std::filesystem::path &workspace)
{
    // No shared or reusable cache: separate mode-0700 environment per TU,
    // with exact post-rewrite MIR, target identity, and rule/schema metadata.
    // Read the committed hint relation before giving it to the backend.
    detail::TempDirectory dbdir(workspace, "factir-XXXXXX");
    auto env = lmdb::env::create();
    env.set_mapsize(64u * 1024u * 1024u);
    env.open(dbdir.path.c_str(), 0, 0600);
    {
        auto txn = lmdb::txn::begin(env);
        auto db = lmdb::dbi::open(txn, nullptr);
        const auto put = [&](const char *key, std::string_view value) {
            lmdb::val bytes(value.data(), value.size());
            if (!db.put(txn, lmdb::val(key), bytes, MDB_NOOVERWRITE))
                throw std::runtime_error("duplicate FactIR database key");
        };
        put("schema", schema);
        put("mir_schema", std::to_string(ARCHON_MIR_SCHEMA_VERSION));
        put("rules", signature);
        put("triple", target.triple);
        put("architecture", target.architecture);
        put("abi", target.abi);
        put("specification", target.specification.string());
        put("input_name", name);
        put("post_rewrite_mir", mir);
        put("facts", result.facts);
        put("observable_input", hints);
        put("known_bits_i64", bits);
        put("backend_coordinates", coordinates);
        txn.commit();
    }
    auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
    auto db = lmdb::dbi::open(txn, nullptr);
    lmdb::val data;
    if (!db.get(txn, lmdb::val("observable_input"), data))
        throw std::runtime_error("missing committed FactIR hints");
    if (data.size()) {
        std::istringstream rows(std::string(data.data(), data.size()));
        rows.imbue(std::locale::classic());
        std::size_t function;
        ArchonValue dst;
        while (rows >> function >> dst)
            result.preferred.emplace_back(function, dst);
    }
    if (!db.get(txn, lmdb::val("known_bits_i64"), data))
        throw std::runtime_error("missing committed FactIR known bits");
    if (data.size()) {
        std::istringstream rows(std::string(data.data(), data.size()));
        rows.imbue(std::locale::classic());
        KnownBitsRow row;
        while (rows >> row.function >> row.value >> row.bits.zero >> row.bits.one)
            result.known_bits.push_back(row);
    }
    if (!db.get(txn, lmdb::val("backend_coordinates"), data))
        throw std::runtime_error("missing committed FactIR backend coordinates");
    if (data.size()) {
        std::istringstream rows(std::string(data.data(), data.size()));
        rows.imbue(std::locale::classic());
        unsigned kind;
        std::size_t function, block;
        std::uint32_t key;
        while (rows >> kind >> function >> block >> key)
            result.coordinates.emplace_back(static_cast<ArchonBackendFactKind>(kind), function, block, key);
        std::sort(result.coordinates.begin(), result.coordinates.end());
    }
    if (!db.get(txn, lmdb::val("post_rewrite_mir"), data))
        throw std::runtime_error("missing committed FactIR input");
    result.source_mir.assign(data.data(), data.size());
    result.triple = target.triple;
    result.architecture = target.architecture;
    result.abi = target.abi;
    result.specification = target.specification;
    if (!db.get(txn, lmdb::val("facts"), data))
        throw std::runtime_error("missing committed FactIR relations");
    result.facts.assign(data.data(), data.size());
}

inline std::optional<Result> run(std::string &mir, const Target &target, const std::string &name,
                                 const std::filesystem::path &workspace, Diagnostics &diag)
{
    Result result;
    Scan candidates{true};
    if (!scan(mir, candidates, diag, name))
        return std::nullopt;
    result.smtlib = "(set-logic QF_BV)\n; Cephyr FactIR: no applicable add-to-copy candidate\n(exit)\n";
    std::string transformed;
    if (!candidates.edits.empty()) {
        // All candidates share exactly this obligation: both operand orders
        // of modular addition lower to the same commutative bit-vector adder.
        const auto proof = prove_add_constant(0);
        result.smtlib = "(set-logic QF_BV)\n"
                        "; Zero operand comes from an i64 constant zero, possibly through\n"
                        "; verified zero-preserving copies and conversions in the same block.\n"
                        "; Is there an i64 counterexample to add x,0 -> copy x?\n"
                        "(declare-fun x () (_ BitVec 64))\n"
                        "(assert (distinct (bvadd x (_ bv0 64)) x))\n"
                        "(check-sat)\n";
        if (proof.unsatisfiable()) {
            result.rewrites = candidates.edits.size();
            std::size_t previous = 0;
            for (const auto &edit : candidates.edits) {
                transformed.append(mir, previous, edit.begin - previous);
                transformed += "copy " + std::to_string(edit.dst) + " " + std::to_string(edit.source);
                previous = edit.end;
            }
            transformed.append(mir, previous);
            result.smtlib += "; Satie: unsat; applied to " + std::to_string(result.rewrites) + " instruction(s)\n";
        } else {
            result.smtlib += proof.satisfiable() ? "; Satie: sat; not applied\n" :
                                                  "; Satie: unknown; not applied\n";
        }
        result.smtlib += "(exit)\n";
    }
    Scan post_rewrite{false};
    const auto &verified_input = result.rewrites ? transformed : mir;
    if (result.rewrites) {
        candidates.facts.clear();
        candidates.known_bits.clear();
        if (!scan(verified_input, post_rewrite, diag, name))
            return std::nullopt;
    }
    const auto &relations = result.rewrites ? post_rewrite : candidates;

    const auto program = rules();
    std::ostringstream dump, hints, bits, coordinates;
    dump.imbue(std::locale::classic());
    hints.imbue(std::locale::classic());
    bits.imbue(std::locale::classic());
    coordinates.imbue(std::locale::classic());
    dump << schema << '\n';
    try {
        if (relations.over_budget)
            throw dl::BudgetExceeded("FactIR input relation budget exceeded");
        for (const auto &relation : {"observable_input", "dependency"}) {
            auto rows = program.evaluate(relations.facts, relation, relation_limits).rows;
            if (std::string_view(relation) == "observable_input")
                for (const auto &row : rows)
                    hints << std::get<std::int64_t>(row[0]) << ' ' << std::get<std::int64_t>(row[1]) << '\n';
            dump << '[' << relation << "]\n" << dl::dump(rows);
        }
        for (const auto &[relation, rows] : relations.facts) {
            const unsigned kind = relation == "control_edge" ? ARCHON_BACKEND_FACT_CONTROL_EDGE :
                relation == "return_value" ? ARCHON_BACKEND_FACT_RETURN_VALUE : 0;
            if (kind)
                for (const auto &row : rows)
                    coordinates << kind << ' ' << std::get<std::int64_t>(row[0]) << ' '
                                << std::get<std::int64_t>(row[1]) << ' '
                                << std::get<std::int64_t>(row[2]) << '\n';
            dump << '[' << relation << "]\n" << dl::dump(rows);
        }
        dump << "[known_bits_i64]\n";
        for (const auto &[f, v, masks] : relations.known_bits) {
            bits << f << ' ' << v << ' ' << masks.zero << ' ' << masks.one << '\n';
            dump << '(' << f << ", " << v << ", " << masks.zero << ", " << masks.one << ")\n";
        }
        result.facts = std::move(dump).str();
    } catch (const dl::BudgetExceeded &) {
        result.budget_exceeded = true;
        result.facts = std::string(schema) + "\nDatalog budget exceeded; no facts exported or consumed\n";
        hints.str({});
        bits.str({});
        coordinates.str({});
    }
    persist(result, target, name, verified_input, program.signature(), hints.str(), bits.str(),
            coordinates.str(), workspace);
    if (result.rewrites)
        mir.swap(transformed);
    return result;
}
} // namespace cephyr::factir

#endif
