#pragma once

#include "CephyrMIRModel.hpp"

#include <functional>
#include <queue>

namespace cephyr::mir {
namespace schedule_detail {

// Build immediate dominators in reverse postorder, then number the dominator
// tree so checking a cross-block use does not walk the tree each time.
inline bool dominator_intervals(const std::vector<std::vector<BlockId>> &out,
                                const std::vector<std::vector<BlockId>> &in,
                                std::vector<std::size_t> &pre, std::vector<std::size_t> &end)
{
    struct Visit {
        BlockId block;
        std::size_t next = 0;
    };
    std::vector<Visit> stack{{0}};
    std::vector<bool> seen(out.size());
    std::vector<BlockId> postorder;
    seen[0] = true;
    while (!stack.empty()) {
        auto &visit = stack.back();
        if (visit.next == out[visit.block].size()) {
            postorder.push_back(visit.block);
            stack.pop_back();
            continue;
        }
        const BlockId next = out[visit.block][visit.next++];
        if (!seen[next]) {
            seen[next] = true;
            stack.push_back({next});
        }
    }
    if (postorder.size() != out.size())
        return false;
    std::vector<std::size_t> rank(out.size());
    for (std::size_t i = 0; i < postorder.size(); ++i)
        rank[postorder[i]] = i;
    std::vector<BlockId> idom(out.size(), no_block);
    idom[0] = 0;
    bool changed;
    do {
        changed = false;
        for (auto it = postorder.rbegin() + 1; it != postorder.rend(); ++it) {
            BlockId dom = no_block;
            for (BlockId pred : in[*it]) {
                if (idom[pred] == no_block)
                    continue;
                if (dom == no_block) {
                    dom = pred;
                    continue;
                }
                while (dom != pred) {
                    if (rank[dom] < rank[pred])
                        dom = idom[dom];
                    else
                        pred = idom[pred];
                }
            }
            if (dom != idom[*it]) {
                idom[*it] = dom;
                changed = true;
            }
        }
    } while (changed);

    std::vector<std::vector<BlockId>> children(out.size());
    for (std::size_t b = 1; b < out.size(); ++b)
        children[idom[b]].push_back(static_cast<BlockId>(b));
    std::size_t point = 0;
    pre[0] = point++;
    stack.push_back({0});
    while (!stack.empty()) {
        auto &visit = stack.back();
        if (visit.next == children[visit.block].size()) {
            end[visit.block] = point;
            stack.pop_back();
        } else {
            const BlockId child = children[visit.block][visit.next++];
            pre[child] = point++;
            stack.push_back({child});
        }
    }
    return true;
}

inline bool has_result(const Node &node)
{
    return node.value != no_value && node.type != ValueType::void_ &&
           !value_type_name(node.type).empty();
}

inline bool bare(const Node &node)
{
    return node.opcode == Opcode::invalid && node.immediate == 0 && node.symbol.empty();
}

inline bool no_result(const Node &node)
{
    return node.value == no_value && node.type == ValueType::void_;
}

} // namespace schedule_detail

// Validate graph-wide structure and produce a stable local topological order.
// Block parameters and terminators have their own wire positions. The caller
// retains responsibility for Archon's target-specific verifier/legality.
inline std::optional<std::vector<std::vector<NodeId>>> schedule(const Graph &graph)
{
    using namespace schedule_detail;
    const auto &nodes = graph.nodes;
    const auto &blocks = graph.blocks;
    const auto node_count = nodes.size(), block_count = blocks.size();
    if (block_count == 0 || block_count >= no_block || node_count >= no_node ||
        graph.start >= node_count || nodes[graph.start].kind != NodeKind::start)
        return std::nullopt;

    std::vector<bool> parameter_seen(node_count);
    for (std::size_t b = 0; b < block_count; ++b) {
        const auto &[region, terminator, parameters] = blocks[b];
        auto params = graph.input_slice(parameters);
        if (!params || (!b && !params->empty()) || region >= node_count ||
            terminator >= node_count || nodes[region].kind != NodeKind::region ||
            nodes[terminator].kind != NodeKind::terminator || nodes[region].block != b ||
            nodes[terminator].block != b)
            return std::nullopt;
        for (const auto &param : *params) {
            if (param.kind != EdgeKind::data || param.source >= node_count ||
                nodes[param.source].kind != NodeKind::parameter ||
                nodes[param.source].block != b || parameter_seen[param.source])
                return std::nullopt;
            parameter_seen[param.source] = true;
        }
    }

    std::vector<std::vector<BlockId>> outgoing(block_count), incoming(block_count);
    std::vector<std::vector<NodeId>> incoming_nodes(block_count);
    incoming_nodes[0].push_back(graph.start);
    for (const auto &edge : graph.successors) {
        if (edge.terminator >= node_count || edge.target == 0 || edge.target >= block_count)
            return std::nullopt;
        const auto &term = nodes[edge.terminator];
        const auto info = opcode_info(term.opcode);
        if (term.kind != NodeKind::terminator || term.block >= block_count ||
            edge.terminator != blocks[term.block].terminator || !info ||
            !info->is_terminator() || outgoing[term.block].size() == info->successors)
            return std::nullopt;
        outgoing[term.block].push_back(edge.target);
        incoming[edge.target].push_back(term.block);
        incoming_nodes[edge.target].push_back(edge.terminator);
    }
    for (std::size_t b = 0; b < block_count; ++b) {
        const auto info = opcode_info(nodes[blocks[b].terminator].opcode);
        if (!info || !info->is_terminator() || info->successors != outgoing[b].size())
            return std::nullopt;
    }
    std::vector<std::size_t> pre(block_count), end(block_count);
    if (!dominator_intervals(outgoing, incoming, pre, end))
        return std::nullopt;

    const auto data_source = [&](NodeId source, BlockId use_block) {
        if (source >= node_count)
            return false;
        const auto &def = nodes[source];
        if (!has_result(def) ||
            (def.kind != NodeKind::parameter && def.kind != NodeKind::operation))
            return false;
        return (def.kind == NodeKind::parameter && def.block == no_block) ||
               (def.block < block_count && pre[def.block] <= pre[use_block] &&
                pre[use_block] < end[def.block]);
    };
    for (const auto &edge : graph.successors) {
        const auto args = graph.input_slice(edge.arguments);
        const auto params = graph.input_slice(blocks[edge.target].parameters);
        if (!args || args->size() != params->size())
            return std::nullopt;
        for (std::size_t i = 0; i < args->size(); ++i) {
            const auto &arg = (*args)[i];
            if (arg.kind != EdgeKind::data || !data_source(arg.source, nodes[edge.terminator].block) ||
                nodes[arg.source].type != nodes[(*params)[i].source].type)
                return std::nullopt;
        }
    }

    std::vector<bool> effect_read(node_count);
    std::vector<ValueId> values, arguments;
    std::size_t operations = 0;
    bool already_ordered = true;
    for (NodeId id = 0; id < node_count; ++id) {
        const auto &node = nodes[id];
        const auto inputs = graph.input_slice(node.inputs);
        if (!inputs)
            return std::nullopt;
        if (node.value != no_value)
            values.push_back(node.value);
        if (node.kind == NodeKind::start) {
            if (id != graph.start || node.block != no_block || !inputs->empty() ||
                !no_result(node) || !bare(node))
                return std::nullopt;
            continue;
        }
        if (node.kind == NodeKind::parameter) {
            if (!has_result(node) || !bare(node) || inputs->size() != 1 ||
                (*inputs)[0].kind != EdgeKind::control)
                return std::nullopt;
            if (node.block == no_block) {
                if ((*inputs)[0].source != graph.start)
                    return std::nullopt;
                arguments.push_back(node.value);
            } else if (node.block >= block_count || !parameter_seen[id] ||
                       (*inputs)[0].source != blocks[node.block].region) {
                return std::nullopt;
            }
            continue;
        }
        if (node.block >= block_count)
            return std::nullopt;
        const auto region = blocks[node.block].region;
        if (node.kind == NodeKind::region) {
            if (id != region || !bare(node) || !no_result(node))
                return std::nullopt;
            auto &expected = incoming_nodes[node.block];
            if (inputs->size() / 2 != expected.size() || inputs->size() % 2)
                return std::nullopt;
            std::vector<NodeId> controls, effects;
            for (const auto &input : *inputs) {
                if (input.kind == EdgeKind::control)
                    controls.push_back(input.source);
                else if (input.kind == EdgeKind::effect)
                    effects.push_back(input.source);
                else
                    return std::nullopt;
            }
            std::sort(expected.begin(), expected.end());
            std::sort(controls.begin(), controls.end());
            std::sort(effects.begin(), effects.end());
            if (expected != controls || expected != effects)
                return std::nullopt;
            continue;
        }
        const bool terminator = node.kind == NodeKind::terminator;
        const auto info = opcode_info(node.opcode);
        if ((!terminator && node.kind != NodeKind::operation) || !info ||
            terminator != info->is_terminator() ||
            (terminator && id != blocks[node.block].terminator))
            return std::nullopt;
        if ((info->result == ResultKind::required && !has_result(node)) ||
            (info->result == ResultKind::none && !no_result(node)) ||
            (info->result == ResultKind::optional && !has_result(node) && !no_result(node)))
            return std::nullopt;
        const bool has_symbol = info->payload == PayloadKind::call || info->payload == PayloadKind::symbol;
        if (has_symbol == node.symbol.empty() ||
            (has_symbol && (node.symbol.find_first_of(" \t\n\r") != std::string::npos ||
                            node.symbol.find('\0') != std::string::npos)) ||
            (info->payload != PayloadKind::word && node.immediate))
            return std::nullopt;
        std::size_t data = 0;
        bool control = false, effect = false;
        for (const auto &input : *inputs) {
            if (input.source >= node_count)
                return std::nullopt;
            const auto &source = nodes[input.source];
            switch (input.kind) {
            case EdgeKind::control:
                if (control || input.source != region)
                    return std::nullopt;
                control = true;
                break;
            case EdgeKind::effect:
                if (effect || effect_read[input.source] || !info->is_ordered())
                    return std::nullopt;
                if (input.source != region) {
                    const auto source_info = opcode_info(source.opcode);
                    if (source.kind != NodeKind::operation || source.block != node.block ||
                        !source_info || !source_info->is_ordered() || source_info->is_terminator())
                        return std::nullopt;
                    if (!terminator && input.source >= id)
                        already_ordered = false;
                }
                effect = effect_read[input.source] = true;
                break;
            case EdgeKind::data:
                if (!data_source(input.source, node.block))
                    return std::nullopt;
                if (node.opcode == Opcode::cbr && source.type != ValueType::i64)
                    return std::nullopt;
                if (!terminator && source.kind == NodeKind::operation &&
                    source.block == node.block && input.source >= id)
                    already_ordered = false;
                ++data;
                break;
            default:
                return std::nullopt;
            }
        }
        if (!info->accepts_inputs(data) || !control || effect != info->is_ordered())
            return std::nullopt;
        operations += !terminator;
    }
    std::sort(values.begin(), values.end());
    if (std::adjacent_find(values.begin(), values.end()) != values.end())
        return std::nullopt;
    std::sort(arguments.begin(), arguments.end());
    for (std::size_t i = 0; i < arguments.size(); ++i)
        if (arguments[i] != i)
            return std::nullopt;

    std::vector<std::vector<NodeId>> result(block_count);
    // The C frontend normally imports instructions already ordered by local
    // dependency. Do not allocate a use graph or heap just to recover it.
    if (already_ordered) {
        for (NodeId id = 0; id < node_count; ++id)
            if (nodes[id].kind == NodeKind::operation)
                result[nodes[id].block].push_back(id);
        return result;
    }

    struct Use {
        NodeId destination;
        std::size_t next;
    };
    constexpr auto no_use = std::numeric_limits<std::size_t>::max();
    std::vector<Use> uses;
    std::vector<std::size_t> first_use(node_count, no_use), dependencies(node_count);
    for (NodeId id = 0; id < node_count; ++id) {
        const auto &node = nodes[id];
        if (node.kind != NodeKind::operation)
            continue;
        const auto edges = graph.input_slice(node.inputs);
        for (const auto &input : *edges) {
            const auto &source = nodes[input.source];
            if ((input.kind == EdgeKind::data || input.kind == EdgeKind::effect) &&
                source.kind == NodeKind::operation && source.block == node.block) {
                uses.push_back({id, first_use[input.source]});
                first_use[input.source] = uses.size() - 1;
                ++dependencies[id];
            }
        }
    }
    std::priority_queue<NodeId, std::vector<NodeId>, std::greater<>> ready;
    for (NodeId id = 0; id < node_count; ++id)
        if (nodes[id].kind == NodeKind::operation && !dependencies[id])
            ready.push(id);
    while (!ready.empty()) {
        const auto id = ready.top();
        ready.pop();
        result[nodes[id].block].push_back(id);
        --operations;
        for (auto use = first_use[id]; use != no_use; use = uses[use].next) {
            const auto dst = uses[use].destination;
            if (--dependencies[dst] == 0)
                ready.push(dst);
        }
    }
    if (operations)
        return std::nullopt;
    return result;
}

} // namespace cephyr::mir
