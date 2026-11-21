#include "moonstone_analysis.hpp"

#include <cephyr/lower.hpp>

#include <chisel/Chisel.hpp>
#include <nerotypical/sequent/Evaluate.hpp>
#include <supersonic/Supersonic.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cephyr::detail {
namespace {

namespace chisel = moonstone::chisel;
namespace sequent = moonstone::nerotypical::sequent;
namespace synthesis = moonstone::supersonic;
using moonstone::Status;

constexpr std::size_t function_limit = 128, instruction_limit = 16384, diagnostic_limit = 32;
constexpr std::size_t cfg_limit = 128, frame_limit = 64, chisel_work = 32768, sequent_limit = 64;

struct Failure {
    Status status;
    std::string_view reason;
    Node *origin = nullptr;
    std::string_view opcode = {};
};

struct Operands {
    std::array<std::string_view, 3> tokens{};
    std::size_t count = 0;

    explicit Operands(std::string_view text)
    {
        while (!text.empty()) {
            const auto split = text.find(' ');
            if (count < tokens.size())
                tokens[count] = text.substr(0, split);
            if (++count > tokens.size() || split == std::string_view::npos)
                break;
            text.remove_prefix(split + 1);
            if (text.empty())
                ++count; // An empty trailing operand is invalid.
        }
    }

    template<class Int>
    std::optional<Int> number(std::size_t i) const
    {
        const auto text = tokens.at(i);
        if (text.empty())
            return std::nullopt;
        Int result{};
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, result);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            return std::nullopt;
        return result;
    }
};

bool integer_value(const Function &function, Id id)
{
    return id < function.values.size() && function.values[id].integer();
}

enum class Operation { constant, copy, extend, arithmetic, load, store, unknown };

struct Opcode {
    Operation kind;
    unsigned bytes = 0;
};

Opcode classify(std::string_view op)
{
    if (op == "const")
        return {Operation::constant};
    if (op == "copy")
        return {Operation::copy};
    if (op == "sext" || op == "zext" || op == "trunc")
        return {Operation::extend};
    constexpr std::array<std::string_view, 12> arithmetic = {
        "add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr", "eq", "slt", "ult",
    };
    // Deliberately allowlist only scalar operations that cannot expose a frame
    // address. Trapping opcodes and pointer arithmetic are not modeled.
    if (std::find(arithmetic.begin(), arithmetic.end(), op) != arithmetic.end())
        return {Operation::arithmetic};
    if (op == "load_s8" || op == "load_u8")
        return {Operation::load, 1};
    if (op == "load_s16" || op == "load_u16")
        return {Operation::load, 2};
    if (op == "load_s32" || op == "load_u32")
        return {Operation::load, 4};
    if (op == "load")
        return {Operation::load, 8};
    if (op == "store8")
        return {Operation::store, 1};
    if (op == "store16")
        return {Operation::store, 2};
    if (op == "store32")
        return {Operation::store, 4};
    if (op == "store")
        return {Operation::store, 8};
    return {Operation::unknown};
}

struct CFG {
    std::vector<Id> order;
};

std::optional<Failure> gather_cfg(const Function &function, CFG &cfg)
{
    const auto n = function.blocks.size();
    if (n == 0)
        return Failure{Status::InvalidInput, "missing entry block"};
    if (n > cfg_limit)
        return Failure{Status::BudgetExhausted, "CFG block limit"};
    std::vector<bool> reachable(n);
    reachable[0] = true;
    cfg.order.push_back(0);
    for (std::size_t i = 0; i < cfg.order.size(); ++i) {
        const auto &term = function.blocks[cfg.order[i]].term;
        const auto edge = [&](const Edge &edge) {
            if (edge.target >= n || edge.args.size() != function.blocks[edge.target].params.size())
                return false;
            if (!reachable[edge.target]) {
                reachable[edge.target] = true;
                cfg.order.push_back(edge.target);
            }
            return true;
        };
        switch (term.kind) {
        case Term::Kind::ret:
        case Term::Kind::trap:
            break;
        case Term::Kind::jump:
            if (!edge(term.yes))
                return Failure{Status::InvalidInput, "invalid CFG edge"};
            break;
        case Term::Kind::branch:
            if (!edge(term.yes) || !edge(term.no))
                return Failure{Status::InvalidInput, "invalid CFG edge"};
            break;
        default:
            return Failure{Status::InvalidInput, "unterminated reachable block"};
        }
    }
    return std::nullopt;
}

struct Access {
    Id place;
    bool store;
    const Instruction *instruction;
};

template<class Findings>
std::optional<Failure> check_frames(const Function &function, const CFG &cfg, Findings finding)
{
    if (function.locals.size() > frame_limit)
        return Failure{Status::BudgetExhausted, "frame slot limit"};
    if (function.entry_addresses.size() != function.locals.size())
        return Failure{Status::InvalidInput, "incomplete frame address table"};
    std::vector<Id> place_for_address(function.values.size(), no_value);
    std::vector<bool> seen_place(function.locals.size());
    for (const auto &address : function.entry_addresses) {
        const Operands operands(address.operands);
        const auto slot = operands.number<Id>(0);
        if (address.opcode != "frame_addr" || address.result >= function.values.size() ||
            operands.count != 1 || !slot || *slot >= function.locals.size() ||
            seen_place[*slot] || place_for_address[address.result] != no_value)
            return Failure{Status::InvalidInput, "invalid frame address table", address.origin};
        const auto &type = function.values[address.result];
        if (!type.pointer() || !type.element || !type.element->integer() || type.element->is_volatile)
            return Failure{Status::Unknown, "non-integer or volatile frame object", address.origin};
        const auto bytes = (type.element->bits + 7) / 8;
        if (!bytes || bytes != function.locals[*slot].first)
            return Failure{Status::InvalidInput, "inconsistent frame object width", address.origin};
        place_for_address[address.result] = *slot;
        seen_place[*slot] = true;
    }

    chisel::Function graph;
    std::vector<std::vector<Access>> accesses(function.blocks.size());
    std::size_t max_transfer = 1;
    for (Id id : cfg.order) {
        const auto &block = function.blocks[id];
        chisel::Block transfer;
        transfer.id = id;
        const auto integers = [&](const std::vector<Id> &values) {
            return std::all_of(values.begin(), values.end(), [&](Id value) { return integer_value(function, value); });
        };
        const auto &term = block.term;
        if (!integers(block.params) ||
            (term.kind == Term::Kind::ret && term.value != no_value && !integer_value(function, term.value)) ||
            (term.kind == Term::Kind::branch && !integer_value(function, term.value)))
            return Failure{Status::Unknown, "non-integer block or terminator operand"};
        const auto edge = [&](const Edge &edge) {
            if (!integers(edge.args))
                return false;
            transfer.successors.push_back(edge.target);
            return true;
        };
        if (((term.kind == Term::Kind::jump || term.kind == Term::Kind::branch) && !edge(term.yes)) ||
            (term.kind == Term::Kind::branch && !edge(term.no)))
            return Failure{Status::Unknown, "non-integer edge argument"};

        std::vector<bool> written(function.locals.size());
        for (const auto &inst : block.instructions) {
            const auto op = classify(inst.opcode);
            if (op.kind == Operation::unknown)
                return Failure{Status::Unknown, "opcode outside the modeled frame effect set", inst.origin, inst.opcode};
            const Operands operands(inst.operands);
            const auto invalid = Failure{Status::InvalidInput, "invalid modeled instruction operands", inst.origin};
            const auto integral = [&](unsigned field) {
                const auto id = operands.number<Id>(field);
                return id && integer_value(function, *id);
            };
            if (op.kind == Operation::load || op.kind == Operation::store) {
                const bool store = op.kind == Operation::store;
                const auto address = operands.number<Id>(0);
                const auto offset = operands.number<std::int64_t>(store ? 2 : 1);
                if (operands.count != (store ? 3u : 2u) || !offset || !address ||
                    *address >= place_for_address.size() ||
                    (store ? inst.result != no_value || !integral(1) : !integer_value(function, inst.result)))
                    return invalid;
                const auto place = place_for_address[*address];
                if (*offset || place == no_value || function.locals[place].first != op.bytes)
                    return Failure{Status::Unknown, "memory access not to a direct whole frame slot", inst.origin};
                // A full write establishes initialization, irrespective of
                // prior writes. Only one write per place matters to block OUT.
                accesses[id].push_back({place, store, &inst});
                if (store && !written[place]) {
                    written[place] = true;
                    transfer.instructions.push_back({chisel::Op::Reinitialize, place, 0, {"", 0, 0, 0, 0}});
                }
            } else {
                if (!integer_value(function, inst.result))
                    return Failure{Status::Unknown, "non-integer result in scalar opcode", inst.origin};
                switch (op.kind) {
                case Operation::constant:
                    if (operands.count != 1 || !operands.number<std::uint64_t>(0))
                        return invalid;
                    break;
                case Operation::copy:
                    if (operands.count != 1 || !integral(0))
                        return invalid;
                    break;
                case Operation::extend: {
                    const auto bits = operands.number<unsigned>(1);
                    if (operands.count != 2 || !integral(0) || !bits || !*bits || *bits > 64)
                        return invalid;
                    break;
                }
                case Operation::arithmetic:
                    if (operands.count != 2 || !integral(0) || !integral(1))
                        return invalid;
                    break;
                default:
                    return invalid;
                }
            }
        }
        max_transfer = std::max(max_transfer, transfer.instructions.size() + function.locals.size());
        graph.blocks.push_back(std::move(transfer));
    }
    if (function.locals.empty())
        return std::nullopt;

    // Give the synthetic graph a separate entry so a jump to the first MIR
    // block cannot discard the initial uninitialized state.
    graph.entry = static_cast<Id>(function.blocks.size());
    graph.blocks.push_back({graph.entry, {}, {0}});
    chisel::Policy policy;
    policy.iteration_limit = std::min<std::size_t>(1024, chisel_work / max_transfer);
    const auto result = chisel::analyze(graph, policy);
    if (!result.converged)
        return Failure{Status::BudgetExhausted, "Chisel fixed-point iteration limit"};
    // Chisel's raw messages reflect its ownership policy and interim solver
    // states. Use only the converged must-live states, then replay local writes
    // and report structural-path findings at the original load locations.
    for (Id id : cfg.order) {
        const auto &incoming = result.block_in.at(id);
        std::vector<bool> written(function.locals.size());
        for (const auto &event : accesses[id]) {
            if (event.store)
                written[event.place] = true;
            else if (!written[event.place] && chisel::at(incoming, event.place).life != chisel::Life::Live)
                finding(event.place, event.instruction->origin);
        }
    }
    return std::nullopt;
}

bool signed_width(const CType &type)
{
    return type.integer() && type.is_signed && (type.bits == 32 || type.bits == 64);
}

bool fits(std::int64_t value, unsigned width)
{
    return width == 64 || (width == 32 && value >= std::numeric_limits<std::int32_t>::min() &&
                            value <= std::numeric_limits<std::int32_t>::max());
}

// Independent of the Supersonic enumerator and the Sequent evaluator. The
// lowering domain only admits operations which are exact at their C width.
std::optional<std::int64_t> checked(std::string_view op, std::int64_t left, std::int64_t right, unsigned width)
{
    // Every pair of signed 64-bit operands fits these operations in signed
    // 128 bits. Use widened arithmetic rather than the enumerator's builtins.
    const auto lhs = static_cast<__int128_t>(left), rhs = static_cast<__int128_t>(right);
    if (op != "add" && op != "sub" && op != "mul")
        return std::nullopt;
    const auto wide = op == "add" ? lhs + rhs : op == "sub" ? lhs - rhs : lhs * rhs;
    if (wide < std::numeric_limits<std::int64_t>::min() || wide > std::numeric_limits<std::int64_t>::max())
        return std::nullopt;
    const auto narrowed = static_cast<std::int64_t>(wide);
    return fits(narrowed, width) ? std::optional{narrowed} : std::nullopt;
}

struct Projection {
    unsigned width;
    std::int64_t value = 0;
    std::size_t operations = 0;
    std::vector<std::int64_t> literals{0, 1, -1};
};

std::optional<Failure> project(const Function &function, const CFG &cfg, Projection &result)
{
    const auto &type = function.symbol.type;
    const auto &block = function.blocks[0];
    if (!type.element || !signed_width(*type.element) || !type.params.empty() ||
        cfg.order.size() != 1 || !block.params.empty() || block.term.kind != Term::Kind::ret ||
        !function.locals.empty() || !function.entry_addresses.empty())
        return Failure{Status::Unknown, "outside the closed, straight-line signed integer subset"};
    if (block.instructions.size() > sequent_limit)
        return Failure{Status::BudgetExhausted, "straight-line Sequent instruction limit"};

    namespace make = sequent::make;
    const auto int_type = sequent::Type::integer();
    const auto binary = sequent::Type::arrow(int_type, sequent::Type::arrow(int_type, int_type));
    sequent::Module module;
    module.result_type = int_type;
    std::vector<std::optional<std::int64_t>> values(function.values.size());
    std::vector<std::pair<Id, sequent::TermPtr>> bindings;
    const auto name = [](Id id) { return "v" + std::to_string(id); };
    const auto variable = [&](Id id) { return make::variable(name(id), int_type); };
    for (const auto &inst : block.instructions) {
        const auto invalid = Failure{Status::InvalidInput, "invalid or unavailable SSA value in projection", inst.origin};
        const auto unsupported = Failure{Status::Unknown, "opcode or type outside the closed integer subset",
                                         inst.origin, inst.opcode};
        if (inst.opcode != "const" && inst.opcode != "copy" && inst.opcode != "sext" &&
            inst.opcode != "add" && inst.opcode != "sub" && inst.opcode != "mul")
            return unsupported;
        if (inst.result >= values.size() || values[inst.result])
            return invalid;
        const auto &dst = function.values[inst.result];
        if (!signed_width(dst))
            return unsupported;
        const Operands operands(inst.operands);
        const auto input = [&](std::size_t i) -> std::optional<Id> {
            const auto id = operands.number<Id>(i);
            return id && *id < values.size() && values[*id] ? id : std::nullopt;
        };
        sequent::TermPtr term;
        std::int64_t value;
        if (inst.opcode == "const") {
            const auto raw = operands.number<std::uint64_t>(0);
            if (operands.count != 1 || !raw)
                return invalid;
            value = std::bit_cast<std::int64_t>(*raw);
            if (!fits(value, dst.bits))
                return invalid;
            if (result.literals.size() < 8 && fits(value, result.width) &&
                std::find(result.literals.begin(), result.literals.end(), value) == result.literals.end())
                result.literals.push_back(value);
            term = make::integer(value);
        } else if (inst.opcode == "copy" || inst.opcode == "sext") {
            const auto id = input(0);
            const bool copy = inst.opcode == "copy";
            if (!id || operands.count != (copy ? 1u : 2u))
                return invalid;
            value = *values[*id];
            const auto width = operands.number<unsigned>(1);
            if (!fits(value, dst.bits) || (copy ? function.values[*id].bits != dst.bits
                                                : !width || !fits(value, *width)))
                return Failure{Status::Unknown, "width conversion is not an identity in the projection", inst.origin};
            term = variable(*id);
        } else if (inst.opcode == "add" || inst.opcode == "sub" || inst.opcode == "mul") {
            const auto left = input(0), right = input(1);
            if (operands.count != 2 || !left || !right || function.values[*left].bits != dst.bits ||
                function.values[*right].bits != dst.bits)
                return invalid;
            const auto sum = checked(inst.opcode, *values[*left], *values[*right], dst.bits);
            if (!sum)
                return Failure{Status::Unknown, "arithmetic would overflow the signed projection width", inst.origin};
            value = *sum;
            const auto native = "$nt." + inst.opcode;
            if (std::none_of(module.globals.begin(), module.globals.end(),
                             [&](const auto &global) { return global.name == native; }))
                module.globals.push_back({native, binary});
            term = make::mu(int_type, make::cut(make::variable(native, binary),
                {{sequent::Frame::apply(variable(*left)), sequent::Frame::apply(variable(*right))}}));
            ++result.operations;
        } else {
            return unsupported;
        }
        values[inst.result] = value;
        bindings.emplace_back(inst.result, std::move(term));
    }
    const auto ret = block.term.value;
    if (ret >= values.size() || !values[ret])
        return Failure{Status::InvalidInput, "missing signed integer return value"};
    if (!fits(*values[ret], result.width))
        return Failure{Status::Unknown, "return conversion would change the signed value"};
    module.entry = make::cut(variable(ret));
    for (auto i = bindings.rbegin(); i != bindings.rend(); ++i)
        module.entry = make::let_value(name(i->first), int_type, std::move(i->second), std::move(module.entry));

    const auto verification = sequent::Verifier{}.check(module);
    if (!verification.ok())
        return Failure{Status::InvalidInput, "Nerotypical rejected the projected Sequent types"};
    constexpr std::size_t fuel = 2048;
    sequent::Evaluator evaluator(fuel);
    try {
        const auto ref = evaluator.run(module, false); // already type-checked
        if (ref.tag != sequent::Value::Tag::Int || ref.integer != *values[ret])
            return Failure{Status::Refuted, "Sequent result disagrees with checked MIR evaluation"};
        result.value = ref.integer;
    } catch (const std::runtime_error &) {
        if (evaluator.steps() > fuel)
            return Failure{Status::BudgetExhausted, "Sequent evaluator fuel limit"};
        return Failure{Status::Unknown, "Sequent reference evaluator failed"};
    }
    return std::nullopt;
}

std::optional<std::int64_t> replay(const synthesis::Expr &expr, unsigned width, std::size_t &operations)
{
    if (expr.op == synthesis::Op::Constant && expr.args.empty() && fits(expr.value, width))
        return expr.value;
    const auto op = expr.op == synthesis::Op::Add ? "add" : expr.op == synthesis::Op::Sub ? "sub" :
                    expr.op == synthesis::Op::Mul ? "mul" : nullptr;
    if (!op || expr.args.size() != 2)
        return std::nullopt;
    ++operations;
    const auto lhs = replay(expr.args[0], width, operations), rhs = replay(expr.args[1], width, operations);
    return lhs && rhs ? checked(op, *lhs, *rhs, width) : std::nullopt;
}

std::string describe(const Failure &failure)
{
    const auto status = failure.status == Status::BudgetExhausted ? "Budget exhausted" :
        failure.status == Status::InvalidInput ? "Invalid input" :
        failure.status == Status::Refuted ? "Refuted" : "Unknown";
    auto message = std::string(status) + ": " + std::string(failure.reason);
    if (!failure.opcode.empty())
        message += " (" + std::string(failure.opcode.substr(0, 64)) + ")";
    return message;
}

} // namespace

void MoonstoneAnalysis::inspect(const Function &function)
{
    if (exhausted_ || diagnostics_count_ > diagnostic_limit)
        return;
    const auto where = [&](Node *origin = nullptr) {
        if (origin) {
            auto at = location(function.unit.source, origin);
            if (at.line)
                return at;
        }
        return location(function.unit.source, function.symbol.definition);
    };
    const auto note = [&](std::string_view code, std::string_view message, Node *origin = nullptr) {
        report(false, code, "in '" + function.source_name + "': " + std::string(message), where(origin));
    };
    const auto budget = [&] {
        exhausted_ = true;
        report(false, "cephyr.moonstone.budget", "Budget exhausted: invocation analysis limit reached", where());
    };
    if (functions_++ == function_limit) {
        budget();
        return;
    }
    CFG cfg;
    if (const auto failure = gather_cfg(function, cfg)) {
        note("cephyr.moonstone.cfg", describe(*failure));
        return;
    }
    std::size_t count = function.entry_addresses.size();
    for (const auto id : cfg.order)
        count += function.blocks[id].instructions.size();
    if (count > instruction_limit - instructions_) {
        budget();
        return;
    }
    instructions_ += count;

    if (const auto failure = check_frames(function, cfg, [&](Id slot, Node *origin) {
            report(true, "cephyr.moonstone.chisel.uninitialized",
                   "Heuristic: load of frame slot " + std::to_string(slot) + " in '" + function.source_name +
                       "' may lack a prior whole-slot store", where(origin));
        }))
        note("cephyr.moonstone.chisel", describe(*failure), failure->origin);

    Projection projection{function.symbol.type.element ? function.symbol.type.element->bits : 0};
    if (const auto failure = project(function, cfg, projection)) {
        note("cephyr.moonstone.nerotypical", describe(*failure), failure->origin);
        return;
    }
    note("cephyr.moonstone.nerotypical", "Verified: typed Sequent and closed MIR return " +
         std::to_string(projection.value));
    if (!projection.operations || diagnostics_count_ > diagnostic_limit)
        return;

    synthesis::Problem problem;
    problem.grammar.constants = projection.literals;
    problem.examples.push_back({{}, projection.value});
    problem.budget = {std::min<std::size_t>(5, 2 * projection.operations - 1), 512};
    problem.predicate = [&](const synthesis::Expr &expr) {
        std::size_t operations = 0;
        const auto value = replay(expr, projection.width, operations);
        return value && *value == projection.value && operations < projection.operations;
    };
    const auto solution = synthesis::solve(problem);
    constexpr std::string_view code = "cephyr.moonstone.supersonic";
    switch (solution.status) {
    case synthesis::SolveStatus::Found:
        if (solution.program && problem.predicate(*solution.program))
            note(code, "Verified: equivalent closed MIR return expression " + solution.program->print() +
                 " (fewer arithmetic operations; not applied)");
        else
            note(code, "Refuted: synthesized expression failed independent replay");
        break;
    case synthesis::SolveStatus::UnsatisfiableWithinBound:
        note(code, "No independently validated cheaper expression within the grammar and cost bound");
        break;
    case synthesis::SolveStatus::BudgetExhausted:
        note(code, "Budget exhausted: synthesis candidate limit");
        break;
    case synthesis::SolveStatus::InvalidInput:
        note(code, "Invalid input: synthesis rejected the closed projection");
        break;
    }
}

void MoonstoneAnalysis::report(bool warning, std::string_view code, std::string_view message, Location where)
{
    if (diagnostics_count_ > diagnostic_limit)
        return;
    if (diagnostics_count_++ == diagnostic_limit) {
        diagnostics_.note("cephyr.moonstone.limit", "Further Moonstone diagnostics suppressed", std::move(where));
    } else if (warning) {
        diagnostics_.warning(code, message, std::move(where));
    } else {
        diagnostics_.note(code, message, std::move(where));
    }
}

} // namespace cephyr::detail
