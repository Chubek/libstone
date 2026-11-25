#include "mir_bridge.hpp"

#include <cephyr/lower.hpp>
#include <cephyr/mir_schedule.hpp>

#include "mir_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <concepts>
#include <limits>

namespace cephyr::detail {
namespace {

using mir::EdgeKind;
using mir::Graph;
using mir::Input;
using mir::NodeId;
using mir::NodeKind;
using mir::Opcode;
using mir::PayloadKind;
using mir::ResultKind;
using mir::ValueType;

constexpr std::size_t max_function_instructions = 16384, max_module_instructions = 65536;
static_assert([] {
    for (const auto &rule : mir::catalog::rules) {
        const auto *op = mir::lookup_opcode(rule.opcode);
        if (!op || op->is_ordered() || op->fixed_inputs != 2 ||
            op->result != ResultKind::required || op->payload != PayloadKind::none)
            return false;
    }
    return true;
}(), "Cephyr MIR catalog rules must match pure binary operations in MIR.acsem");

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::logic_error(message);
}

class Operands {
public:
    explicit Operands(std::string_view text) : text_(text) {}

    std::string_view next()
    {
        const auto first = text_.find_first_not_of(' ');
        if (first == std::string_view::npos) {
            text_ = {};
            return {};
        }
        text_.remove_prefix(first);
        const auto end = text_.find(' ');
        auto result = text_.substr(0, end);
        text_.remove_prefix(result.size());
        return result;
    }

    template<typename T> T number()
    {
        const auto token = next();
        require(!token.empty(), "missing Cephyr MIR operand");
        T value{};
        const auto status = std::from_chars(token.data(), token.data() + token.size(), value);
        require(status.ec == std::errc{} && status.ptr == token.data() + token.size(),
                "invalid numeric Cephyr MIR operand");
        return value;
    }
private:
    std::string_view text_;
};

Opcode term_opcode(Term::Kind kind)
{
    switch (kind) {
    case Term::Kind::ret: return Opcode::ret;
    case Term::Kind::jump: return Opcode::br;
    case Term::Kind::branch: return Opcode::cbr;
    case Term::Kind::trap: return Opcode::trap;
    case Term::Kind::open: break;
    }
    throw std::logic_error("unterminated reachable block from the C frontend");
}

class Import {
public:
    const Function &source;
    Graph graph;
    std::vector<NodeId> arguments;

    explicit Import(const Function &f, std::size_t instruction_budget)
        : source(f), values_(f.values.size(), mir::no_node),
          blocks_(f.blocks.size(), mir::no_block)
    {
        const auto reachable = reachable_blocks(f.blocks);
        graph.nodes.reserve(f.values.size() + f.blocks.size() * 2 + 1);
        graph.inputs.reserve(f.values.size() * 3 + f.blocks.size() * 6);
        graph.start = graph.add_node(mir::Node{.kind = NodeKind::start});
        const auto count = abi_parameter_types(f.symbol.type, true).size();
        arguments.reserve(count);
        for (Id i = 0; i < count; ++i) {
            const auto id = definition(NodeKind::parameter, mir::no_block, i);
            graph.nodes[id].inputs = graph.add_inputs({{graph.start, EdgeKind::control}});
            arguments.push_back(id);
        }
        // Give all definitions stable graph IDs before we connect data edges.
        // Dominance does not require the originating CFG's source order.
        for (std::size_t i = 0; i < f.blocks.size(); ++i) {
            if (!reachable[i])
                continue;
            const auto block = static_cast<mir::BlockId>(graph.blocks.size());
            blocks_[i] = block;
            auto region = graph.add_node(mir::Node{.kind = NodeKind::region, .block = block});
            const auto &src = f.blocks[i];
            scratch_.clear();
            for (const auto value : src.params) {
                const auto id = definition(NodeKind::parameter, block, value);
                graph.nodes[id].inputs = graph.add_inputs({{region, EdgeKind::control}});
                scratch_.push_back({id, EdgeKind::data});
            }
            const auto params = graph.add_inputs(scratch_);
            const auto first = static_cast<NodeId>(graph.nodes.size());
            const auto count = src.instructions.size() + (i == 0 ? f.entry_addresses.size() : 0);
            if (count > instruction_budget)
                throw std::length_error("native backend instruction limit exceeded");
            instruction_budget -= count;
            if (i == 0)
                for (const auto &inst : f.entry_addresses)
                    definition(NodeKind::operation, block, inst.result);
            for (const auto &inst : src.instructions)
                definition(NodeKind::operation, block, inst.result);
            const auto term = graph.add_node(mir::Node{.kind = NodeKind::terminator,
                .block = block, .opcode = term_opcode(src.term.kind)});
            graph.blocks.push_back({region, term, params});
            first_operations_.push_back(first);
        }

        // An incoming edge is a scheduled block's control *and* effect
        // token from its predecessor. Carried data live on the successor.
        std::vector<std::vector<Input>> incoming(graph.blocks.size());
        incoming.at(0).push_back({graph.start, EdgeKind::control});
        incoming[0].push_back({graph.start, EdgeKind::effect});
        for (std::size_t i = 0; i < f.blocks.size(); ++i) {
            const auto block = blocks_[i];
            if (block == mir::no_block)
                continue;
            const auto &src = f.blocks[i];
            const auto &dst = graph.blocks[block];
            auto effect = dst.region;
            auto next = first_operations_[block];
            if (i == 0)
                for (const auto &inst : f.entry_addresses)
                    instruction(next++, inst, dst.region, effect);
            for (const auto &inst : src.instructions)
                instruction(next++, inst, dst.region, effect);

            scratch_.clear();
            if (src.term.value != no_value)
                scratch_.push_back(data(src.term.value));
            scratch_.push_back({dst.region, EdgeKind::control});
            scratch_.push_back({effect, EdgeKind::effect});
            graph.nodes[dst.terminator].inputs = graph.add_inputs(scratch_);
            const auto &info = *mir::opcode_info(graph.nodes[dst.terminator].opcode);
            if (info.successors >= 1)
                successor(dst.terminator, src.term.yes, incoming);
            if (info.successors == 2)
                successor(dst.terminator, src.term.no, incoming);
        }
        for (std::size_t b = 0; b < incoming.size(); ++b)
            graph.nodes[graph.blocks[b].region].inputs = graph.add_inputs(incoming[b]);
    }

private:
    std::vector<NodeId> values_;
    std::vector<mir::BlockId> blocks_;
    std::vector<NodeId> first_operations_;
    std::vector<Input> scratch_;

    NodeId definition(NodeKind kind, mir::BlockId block, Id value)
    {
        mir::Node node{.kind = kind, .block = block, .value = value};
        if (value != no_value) {
            node.type = mir::lookup_value_type(mir_type(source.values.at(value)));
            require(node.type != ValueType::invalid && node.type != ValueType::void_,
                    "unknown Cephyr MIR value type");
        }
        const auto id = graph.add_node(std::move(node));
        if (value != no_value) {
            require(values_.at(value) == mir::no_node, "duplicate Cephyr MIR definition");
            values_[value] = id;
        }
        return id;
    }

    Input data(Id value) const
    {
        const auto id = values_.at(value);
        require(id != mir::no_node, "missing Cephyr MIR definition");
        return {id, EdgeKind::data};
    }

    void instruction(NodeId id, const Instruction &source_inst, NodeId region,
                     NodeId &effect)
    {
        auto &node = graph.nodes[id];
        const auto *info = mir::lookup_opcode(source_inst.opcode);
        require(info && !info->is_terminator(), "unknown Cephyr MIR operation");
        node.opcode = info->opcode;
        Operands operands(source_inst.operands);
        const bool result = node.value != no_value;
        require((info->result != ResultKind::required || result) &&
                (info->result != ResultKind::none || !result),
                "invalid Cephyr MIR result slot");
        if (info->result == ResultKind::optional && !result)
            require(operands.next() == "-", "missing Cephyr MIR void result marker");
        scratch_.clear();
        for (unsigned i = 0; i < info->fixed_inputs; ++i)
            scratch_.push_back(data(operands.number<Id>()));
        switch (info->payload) {
        case PayloadKind::none: break;
        case PayloadKind::word:
            node.immediate = operands.number<std::uint64_t>();
            break;
        case PayloadKind::symbol:
        case PayloadKind::call:
            node.symbol = operands.next();
            require(!node.symbol.empty(), "missing Cephyr MIR symbol");
            if (info->payload == PayloadKind::call) {
                const auto count = operands.number<unsigned>();
                for (unsigned i = 0; i < count; ++i)
                    scratch_.push_back(data(operands.number<Id>()));
            }
            break;
        case PayloadKind::return_value:
            throw std::logic_error("Cephyr MIR terminator used as instruction");
        }
        require(operands.next().empty(), "extra Cephyr MIR operand");
        scratch_.push_back({region, EdgeKind::control});
        if (info->is_ordered()) {
            scratch_.push_back({effect, EdgeKind::effect});
            effect = id;
        }
        node.inputs = graph.add_inputs(scratch_);
    }

    void successor(NodeId term, const Edge &edge, std::vector<std::vector<Input>> &incoming)
    {
        const auto block = blocks_.at(edge.target);
        require(block != mir::no_block, "invalid Cephyr MIR successor");
        scratch_.clear();
        for (const auto arg : edge.args)
            scratch_.push_back(data(arg));
        graph.successors.push_back({term, block, graph.add_inputs(scratch_)});
        incoming[block].push_back({term, EdgeKind::control});
        incoming[block].push_back({term, EdgeKind::effect});
    }
};

std::span<const Input> inputs(const Graph &graph, mir::InputRange range)
{
    auto slice = graph.input_slice(range);
    require(slice.has_value(), "invalid Cephyr MIR edge range");
    return *slice;
}

// The Archon format uses only ASCII tokens and unsigned decimal words.
// Avoid locale-dependent stream formatting on the per-instruction hot path.
class WireText {
public:
    explicit WireText(std::size_t estimate) { text_.reserve(estimate); }

    WireText &operator<<(std::string_view token)
    {
        text_.append(token);
        return *this;
    }

    WireText &operator<<(char c)
    {
        text_.push_back(c);
        return *this;
    }

    template<std::unsigned_integral T> WireText &operator<<(T value)
    {
        char digits[std::numeric_limits<T>::digits10 + 1];
        const auto converted = std::to_chars(digits, digits + sizeof digits, value);
        text_.append(digits, converted.ptr);
        return *this;
    }

    std::string finish() && { return std::move(text_); }

private:
    std::string text_;
};

// Built from the graph, not the construction CFG.
class Wire {
public:
    explicit Wire(const Import &import) : graph_(import.graph), args_(import.arguments) {}

    void verify()
    {
        auto schedule = mir::schedule(graph_);
        require(schedule.has_value(), "invalid Cephyr MIR graph");
        scheduled_ = std::move(*schedule);
    }

    void project_types()
    {
        projected_.resize(graph_.nodes.size());
        for (std::size_t i = 0; i < graph_.nodes.size(); ++i)
            if (graph_.nodes[i].value != no_value) {
                projected_[i] = mir::value_type_name(graph_.nodes[i].type);
                require(!projected_[i].empty(), "invalid Archon MIR type projection");
            }
    }

    std::size_t linearize()
    {
        std::size_t instructions = 0;
        linear_.reserve(graph_.nodes.size());
        for (const auto &block : scheduled_) {
            offsets_.push_back(linear_.size());
            linear_.insert(linear_.end(), block.begin(), block.end());
            instructions += block.size();
        }
        offsets_.push_back(linear_.size());
        return instructions;
    }

    void assign_ids()
    {
        ids_.resize(graph_.nodes.size(), no_value);
        for (const auto arg : args_)
            assign(arg);
        for (std::size_t b = 0; b < graph_.blocks.size(); ++b) {
            const auto &block = graph_.blocks[b];
            for (const auto &param : inputs(graph_, block.parameters))
                assign(param.source);
            for (auto id : operations(b))
                if (graph_.nodes[id].value != no_value)
                    assign(id);
        }
    }

    std::string emit(const Function &f, std::string_view name) const
    {
        WireText out(128 + linear_.size() * 24 + graph_.blocks.size() * 32 + f.locals.size() * 16);
        out << "function " << name << ' ' << mir_type(abi_result_type(*f.symbol.type.element)) << ' '
            << args_.size() << ' ' << types_.size() << ' ' << graph_.blocks.size()
            << ' ' << f.locals.size() << "\ntypes";
        for (const auto type : types_)
            out << ' ' << type;
        out << '\n';
        for (const auto &[size, alignment] : f.locals)
            out << "local " << size << ' ' << alignment << '\n';

        std::size_t next_successor = 0;
        for (std::size_t b = 0; b < graph_.blocks.size(); ++b) {
            const auto &block = graph_.blocks[b];
            const auto ops = operations(b);
            out << "block " << block.parameters.count << ' ' << ops.size() << "\nparams";
            for (const auto &param : inputs(graph_, block.parameters))
                operand(out, param.source);
            out << '\n';
            for (auto id : ops)
                operation(out, id);
            const auto &term = graph_.nodes[block.terminator];
            const auto &info = *mir::opcode_info(term.opcode);
            const auto edges = inputs(graph_, term.inputs);
            out << info.name;
            if (term.opcode == Opcode::ret) {
                if (edges.size() == 2)
                    out << " -";
                else
                    operand(out, edges.front().source);
            } else if (term.opcode == Opcode::cbr) {
                operand(out, edges.front().source);
            }
            for (unsigned s = 0; s < info.successors; ++s) {
                const auto &edge = graph_.successors[next_successor++];
                out << ' ' << edge.target << ' ' << edge.arguments.count;
                for (const auto &arg : inputs(graph_, edge.arguments))
                    operand(out, arg.source);
            }
            out << '\n';
        }
        return std::move(out).finish();
    }

private:
    const Graph &graph_;
    std::span<const NodeId> args_;
    std::vector<std::string_view> projected_, types_;
    std::vector<std::vector<NodeId>> scheduled_;
    std::vector<NodeId> linear_;
    std::vector<std::size_t> offsets_;
    std::vector<Id> ids_;

    std::span<const NodeId> operations(std::size_t block) const
    {
        return std::span{linear_}.subspan(offsets_[block], offsets_[block + 1] - offsets_[block]);
    }

    void assign(NodeId id)
    {
        ids_[id] = static_cast<Id>(types_.size());
        types_.push_back(projected_[id]);
    }

    void operand(WireText &out, NodeId id) const
    {
        require(ids_.at(id) != no_value, "missing Archon MIR value ID");
        out << ' ' << ids_[id];
    }

    void operation(WireText &out, NodeId id) const
    {
        const auto &node = graph_.nodes[id];
        const auto &info = *mir::opcode_info(node.opcode);
        const auto all = inputs(graph_, node.inputs);
        const auto data = all.first(all.size() - (info.is_ordered() ? 2 : 1));
        out << info.name;
        if (info.result != ResultKind::none) {
            if (node.value != no_value)
                operand(out, id);
            else
                out << " -";
        }
        for (const auto &edge : data.first(info.fixed_inputs))
            operand(out, edge.source);
        if (info.payload == PayloadKind::word)
            out << ' ' << node.immediate;
        else if (info.payload == PayloadKind::symbol || info.payload == PayloadKind::call)
            out << ' ' << node.symbol;
        if (info.payload == PayloadKind::call) {
            const auto args = data.subspan(info.fixed_inputs);
            out << ' ' << args.size();
            for (const auto &arg : args)
                operand(out, arg.source);
        }
        out << '\n';
    }
};

bool optimize_graph(Graph &graph, std::uint64_t pass_mask)
{
    using namespace mir::catalog;
    // Match the historical --mir-opt bundle. A named list explicitly selects
    // its rules, including any catalog passes above the bundle's level.
    const unsigned level = pass_mask == mir::catalog::detail::all_ones ? 2 : 3;
    bool changed = false;
    std::vector<std::optional<std::uint64_t>> constants(graph.nodes.size());
    for (std::size_t id = 0; id < graph.nodes.size(); ++id) {
        auto &node = graph.nodes[id];
        if (node.kind != NodeKind::operation || node.type != ValueType::i64)
            continue;
        const auto &info = *mir::opcode_info(node.opcode);
        auto at = graph.inputs.begin() + node.inputs.first;
        if (node.opcode == Opcode::const_) {
            constants[id] = node.immediate;
            continue;
        }
        // C lowering inserts explicit carrier conversions even for full-width
        // operands. Preserve the conversions but see through them for facts.
        if (node.opcode == Opcode::copy || node.opcode == Opcode::zext ||
            node.opcode == Opcode::sext || node.opcode == Opcode::trunc) {
            auto value = constants[at[0].source];
            if (!value || graph.nodes[at[0].source].type != ValueType::i64)
                continue;
            if (node.opcode != Opcode::copy) {
                const auto width = node.immediate;
                if (width == 0 || width > 64)
                    continue;
                if (width < 64) {
                    const auto mask = (std::uint64_t{1} << width) - 1;
                    *value &= mask;
                    if (node.opcode == Opcode::sext && (*value & (std::uint64_t{1} << (width - 1))))
                        *value |= ~mask;
                }
            }
            constants[id] = value;
            continue;
        }
        if (info.is_ordered() || info.fixed_inputs != 2)
            continue;
        const auto a = at[0].source, b = at[1].source;
        const auto &lhs = graph.nodes[a], &rhs = graph.nodes[b];
        auto replacement = rewrite_binary(info.name,
            lhs.type == ValueType::i64 && rhs.type == ValueType::i64, a == b,
            constants[a], constants[b], level, pass_mask);
        if (!replacement)
            continue;
        changed = true;
        // A rewrite replaces just the result-producing node. Do not remove
        // producers (including loads) or alter the graph's control/effects.
        if (replacement->kind == ReplacementKind::Constant) {
            node.opcode = Opcode::const_;
            node.immediate = replacement->bits;
            at[0] = at[2];
            node.inputs.count = 1;
            constants[id] = replacement->bits;
        } else {
            node.opcode = Opcode::copy;
            if (replacement->kind == ReplacementKind::Rhs)
                at[0] = at[1];
            at[1] = at[2];
            node.inputs.count = 2;
            constants[id] = constants[at[0].source];
        }
    }
    return changed;
}

} // namespace

std::string lower_mir_function(const Function &f, std::string_view name,
                                std::size_t &module_instructions, bool optimize,
                                bool &mir_rewritten, std::uint64_t pass_mask)
{
    Import imported(f, std::min(max_function_instructions, max_module_instructions - module_instructions));
    Wire wire(imported);
    std::string text;
    mir::catalog::run_lowering_stages([&](const auto &stage) {
        using mir::catalog::StageKind;
        switch (stage.kind) {
        case StageKind::VerifyGraph:
            wire.verify();
            if (optimize && pass_mask && optimize_graph(imported.graph, pass_mask))
                mir_rewritten = true;
            break;
        case StageKind::ProjectTypes: wire.project_types(); break;
        case StageKind::LinearizeBlocks: {
            const auto instructions = wire.linearize();
            if (instructions > max_function_instructions ||
                instructions > max_module_instructions - module_instructions)
                throw std::length_error("native backend instruction limit exceeded");
            module_instructions += instructions;
            break;
        }
        case StageKind::AssignWireIds: wire.assign_ids(); break;
        case StageKind::EmitArchonMir: text = wire.emit(f, name); break;
        }
        return true;
    });
    return text;
}

} // namespace cephyr::detail
