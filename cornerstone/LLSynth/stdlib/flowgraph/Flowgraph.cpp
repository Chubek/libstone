// Private scalar C ABI for std.flowgraph. The generated runtime copies strings.
#include "dataflow-analysis/Backwards.hpp"
#include "dataflow-analysis/Forwards.hpp"
#include "dataflow-analysis/SetLattice.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace df = wetstone::df;
namespace ir = wetstone::ir;

constexpr std::size_t word_width = 8;
constexpr std::size_t max_blocks = 512;
constexpr std::size_t max_input_words = 8192;
constexpr std::size_t max_output_words = 32768;
constexpr std::size_t max_evaluations = 100000;
constexpr char evaluation_limit[] = "fixed-point evaluation limit exhausted";
constexpr char output_limit[] = "flowgraph result is too large";
thread_local std::string reply;

std::uint32_t parse_word(std::string_view text) {
    if (text.size() != word_width)
        throw std::invalid_argument("truncated flowgraph word");
    std::uint32_t word;
    const auto* end = text.data() + word_width;
    auto parsed = std::from_chars(text.data(), end, word, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end || word == ir::invalid_value)
        throw std::invalid_argument("invalid flowgraph word");
    return word;
}

void write_word(std::string& text, std::uint32_t word) {
    if (text.size() >= max_output_words * word_width)
        throw std::length_error(output_limit);
    char digits[word_width];
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = word_width; i != 0; --i) {
        digits[i - 1] = hex[word & 15];
        word >>= 4;
    }
    text.append(digits, sizeof digits);
}

class Reader {
    std::string_view input_;

public:
    explicit Reader(std::string_view input) : input_(input) {
        if (input_.size() > max_input_words * word_width)
            throw std::invalid_argument("flowgraph request is too large");
        if (input_.size() % word_width != 0)
            throw std::invalid_argument("truncated flowgraph request");
    }

    bool empty() const { return input_.empty(); }

    std::uint32_t word() {
        auto value = parse_word(input_.substr(0, word_width));
        input_.remove_prefix(word_width);
        return value;
    }

    std::size_t count() {
        auto size = word();
        if (size > input_.size() / word_width)
            throw std::invalid_argument("invalid list length in flowgraph request");
        return size;
    }

    std::vector<std::uint32_t> list() {
        auto size = count();
        std::vector<std::uint32_t> result;
        result.reserve(size);
        while (size-- > 0)
            result.push_back(word());
        return result;
    }
};

struct Step {
    std::vector<ir::ValueId> uses, defines;
};

struct Flowgraph {
    ir::Function cfg;
    std::vector<std::vector<Step>> steps;
};

Flowgraph read_graph(std::string_view text) {
    Reader input(text);
    auto count = input.count();
    if (count == 0 || count > max_blocks)
        throw std::invalid_argument("expected 1 to 512 blocks");
    auto entry = input.word();
    if (entry >= count)
        throw std::invalid_argument("entry block is outside the flowgraph");

    Flowgraph graph;
    graph.cfg.entry = entry;
    graph.cfg.blocks.reserve(count);
    graph.steps.resize(count);
    for (ir::BlockId b = 0; b < count; ++b) {
        ir::Instruction terminator;
        terminator.targets = input.list();
        for (auto target : terminator.targets)
            if (target >= count)
                throw std::invalid_argument("successor block is outside the flowgraph");
        switch (terminator.targets.size()) {
        case 0:
            terminator.opcode = ir::Opcode::Return;
            break;
        case 1:
            terminator.opcode = ir::Opcode::Jump;
            break;
        case 2:
            if (terminator.targets[0] == terminator.targets[1])
                throw std::invalid_argument("duplicate flowgraph successor");
            terminator.opcode = ir::Opcode::Branch;
            // Only Wetstone's structural CFG is used, not its branch semantics.
            terminator.arguments.push_back(ir::Operand::constant(0));
            break;
        default:
            throw std::invalid_argument("block has more than two successors");
        }
        graph.cfg.blocks.push_back({b, {std::move(terminator)}});

        auto number_of_steps = input.count();
        auto& steps = graph.steps[b];
        steps.reserve(number_of_steps);
        while (number_of_steps-- > 0)
            steps.push_back({input.list(), input.list()});
    }
    if (!input.empty())
        throw std::invalid_argument("trailing data in flowgraph request");
    return graph;
}

using Values = df::MaySet<ir::ValueId>;

df::Result<Values> liveness(const Flowgraph& graph, std::size_t limit) {
    return df::backwards<Values>(graph.cfg, [&](ir::BlockId b, Values state) {
        const auto& steps = graph.steps[b];
        for (auto step = steps.rbegin(); step != steps.rend(); ++step) {
            for (auto value : step->defines)
                state.values.erase(value);
            state.values.insert(step->uses.begin(), step->uses.end());
        }
        return state;
    }, {}, limit);
}

// One source definition per (block, step, variable). The forward transfer
// kills earlier definitions of the same variable, including those in its block.
using Definition = std::tuple<ir::BlockId, std::uint32_t, ir::ValueId>;
using Definitions = df::MaySet<Definition>;

df::Result<Definitions> reaching_definitions(const Flowgraph& graph, std::size_t limit) {
    std::vector<std::map<ir::ValueId, std::uint32_t>> generated(graph.steps.size());
    for (std::size_t b = 0; b < graph.steps.size(); ++b)
        for (std::uint32_t i = 0; i < graph.steps[b].size(); ++i)
            for (auto value : graph.steps[b][i].defines)
                generated[b][value] = i;

    return df::forwards<Definitions>(graph.cfg, [&](ir::BlockId b, Definitions state) {
        const auto& generate = generated[b];
        std::erase_if(state.values, [&](const auto& def) {
            return generate.contains(std::get<2>(def));
        });
        for (auto [value, i] : generate)
            state.values.emplace(b, i, value);
        return state;
    }, Definitions::bottom(), limit);
}

void write_fact(std::string& output, ir::ValueId value) { write_word(output, value); }

void write_fact(std::string& output, const Definition& definition) {
    auto [block, step, value] = definition;
    write_word(output, block);
    write_word(output, step);
    write_word(output, value);
}

template<class Set>
std::string write_result(const df::Result<Set>& facts, std::size_t blocks) {
    if (!facts.converged)
        return evaluation_limit;
    std::string output;
    for (ir::BlockId b = 0; b < blocks; ++b) {
        const auto in = facts.in.find(b);
        if (in == facts.in.end()) {
            write_word(output, 0);
            write_word(output, 0);
            write_word(output, 0);
            continue;
        }
        const auto& out = facts.out.at(b);
        write_word(output, 1);
        write_word(output, static_cast<std::uint32_t>(in->second.values.size()));
        write_word(output, static_cast<std::uint32_t>(out.values.size()));
        for (const auto& fact : in->second.values)
            write_fact(output, fact);
        for (const auto& fact : out.values)
            write_fact(output, fact);
    }
    return output;
}
} // namespace

extern "C" const char* llsynth_flowgraph_v1_word(std::int64_t value) noexcept {
    if (value < 0 || value >= ir::invalid_value)
        return "";
    try {
        std::string output;
        write_word(output, static_cast<std::uint32_t>(value));
        reply = std::move(output);
        return reply.c_str();
    } catch (...) {
        return "";
    }
}

extern "C" const char* llsynth_flowgraph_v1_concat(const char* left, const char* right) noexcept {
    try {
        if (!left || !right)
            return "";
        const std::string_view a(left), b(right);
        constexpr auto max_bytes = max_input_words * word_width;
        if (a.size() > max_bytes || b.size() > max_bytes - a.size())
            return "";
        std::string merged(a);
        merged.append(b);
        reply = std::move(merged);
        return reply.c_str();
    } catch (...) {
        return "";
    }
}

extern "C" std::int64_t llsynth_flowgraph_v1_read(const char* raw, std::int64_t at) noexcept {
    if (!raw || at < 0 || at >= static_cast<std::int64_t>(max_output_words))
        return -1;
    try {
        return parse_word(std::string_view(raw).substr(static_cast<std::size_t>(at) * word_width,
                                                       word_width));
    } catch (...) {
        return -1;
    }
}

extern "C" const char* llsynth_flowgraph_v1_analyze(const char* raw, std::int64_t kind,
                                                 std::int64_t limit) noexcept {
    try {
        if (!raw)
            return "missing flowgraph request";
        if (limit < 0 || limit > static_cast<std::int64_t>(max_evaluations))
            return "evaluation limit must be in 0..100000";
        const auto graph = read_graph(raw);
        const auto size = graph.steps.size();
        const auto budget = static_cast<std::size_t>(limit);
        switch (kind) {
        case 0:
            reply = write_result(liveness(graph, budget), size);
            break;
        case 1:
            reply = write_result(reaching_definitions(graph, budget), size);
            break;
        default:
            return "unknown flowgraph analysis";
        }
        return reply.c_str();
    } catch (const std::length_error&) {
        return output_limit;
    } catch (const std::invalid_argument& error) {
        try {
            reply = error.what();
            return reply.c_str();
        } catch (...) {
            return "invalid flowgraph request";
        }
    } catch (...) {
        return "flowgraph analysis failed";
    }
}
