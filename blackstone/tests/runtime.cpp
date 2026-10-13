#include "blackstone.hpp"
extern "C" {
#include <lua.h>
int blackstone_c_api_test(const char*);
}
#include <filesystem>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace firestone::ir;
static void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> static void rejects(F action, const std::string& expected) {
    try { action(); } catch (const std::exception& e) {
        check(std::string(e.what()).find(expected) != std::string::npos,
              std::string("wrong error: ") + e.what()); return;
    }
    throw std::runtime_error("expected rejection: " + expected);
}
static void write(const std::filesystem::path& file, const std::string& source) {
    std::ofstream stream(file); stream << source;
    check(static_cast<bool>(stream), "could not write fixture");
}
static std::string kernel(const std::string& name, const std::string& body,
                          const std::string& requirements="{}") {
    return "lrewrite.kernel.define {name='" + name +
           "',ir='ssa',phase='test',requires=" + requirements + ",preserves={'cfg'}}\n"
           "lrewrite.kernel.run=function(ctx) " + body + " end\n";
}
static SimpleSSAGraph loop_graph(unsigned bits=8, int initial=0, int step=1,
                                int scale=7, bool multiple_latches=false) {
    SimpleSSAGraph graph; auto& cfg = graph.cfg();
    auto entry = cfg.create_block("entry"), header = cfg.create_block("header");
    auto body = cfg.create_block("body"), exit = cfg.create_block("exit");
    auto type = Type::integer(bits);
    auto limit = graph.parameter(type);
    auto selector = multiple_latches ? graph.parameter(Type::boolean()) : invalid_id;
    auto iv = graph.reserve_value(type), sum = graph.reserve_value(type);
    auto sum_next = graph.reserve_value(type);
    std::vector<BlockId> latches = {body};
    if (multiple_latches) latches = {cfg.create_block("latch1"), cfg.create_block("latch2")};
    std::vector<ValueId> next;
    for (auto latch : latches) { (void)latch; next.push_back(graph.reserve_value(type)); }
    std::vector<PhiInput> iv_inputs{{entry, Operand::constant(initial, type)}};
    std::vector<PhiInput> sum_inputs{{entry, Operand::constant(0, type)}};
    for (size_t n=0; n<latches.size(); ++n) {
        iv_inputs.push_back({latches[n], Operand::ref(next[n])});
        sum_inputs.push_back({latches[n], Operand::ref(sum_next)});
    }
    cfg.jump(entry, header);
    graph.phi_reserved(header, iv, iv_inputs); graph.phi_reserved(header, sum, sum_inputs);
    auto condition = graph.define(header, Opcode::Lt, Type::boolean(), {Operand::ref(iv), Operand::ref(limit)});
    cfg.branch(header, Operand::ref(condition), body, exit);
    auto product = graph.define(body, Opcode::Mul, type, {Operand::ref(iv), Operand::constant(scale, type)});
    graph.define_reserved(body, sum_next, Opcode::Add, {Operand::ref(sum), Operand::ref(product)});
    if (multiple_latches) cfg.branch(body, Operand::ref(selector), latches[0], latches[1]);
    for (size_t n=0; n<latches.size(); ++n) {
        graph.define_reserved(latches[n], next[n], Opcode::Add, {Operand::ref(iv), Operand::constant(step, type)});
        cfg.jump(latches[n], header);
    }
    cfg.return_value(exit, Operand::ref(sum));
    check(graph.verify().empty(), "fixture SSA is invalid"); return graph;
}
// Independent modular evaluator used to compare observable return values.
static std::uint64_t evaluate(const SimpleSSAGraph& graph, unsigned limit, bool selector=false) {
    std::map<ValueId, std::uint64_t> values{{0, limit}, {1, selector}};
    auto operand = [&](const Operand& o) -> std::uint64_t {
        return o.kind == OperandKind::Value ? values.at(o.value) : static_cast<std::uint64_t>(o.integer);
    };
    auto block = graph.cfg().entry(), previous = invalid_id;
    for (unsigned steps=0; steps<10000; ++steps) {
        const auto& instructions = graph.cfg().block(block).instructions;
        auto phi_values = values;
        for (const auto& inst : instructions) {
            if (inst.opcode != Opcode::Phi) break;
            auto found = std::find(inst.incoming_blocks.begin(), inst.incoming_blocks.end(), previous);
            check(found != inst.incoming_blocks.end(), "missing phi predecessor");
            phi_values[*inst.result] = operand(inst.operands[found-inst.incoming_blocks.begin()]);
        }
        values = std::move(phi_values);
        bool jumped = false;
        for (const auto& inst : instructions) {
            if (inst.opcode == Opcode::Phi) continue;
            if (inst.opcode == Opcode::Return) return operand(inst.operands.at(0));
            if (inst.opcode == Opcode::Jump || inst.opcode == Opcode::Branch) {
                previous = block; block = inst.targets[inst.opcode == Opcode::Jump || operand(inst.operands[0]) ? 0 : 1];
                jumped = true; break;
            }
            std::uint64_t result;
            switch (inst.opcode) {
            case Opcode::Copy: result = operand(inst.operands[0]); break;
            case Opcode::Add: result = operand(inst.operands[0]) + operand(inst.operands[1]); break;
            case Opcode::Mul: result = operand(inst.operands[0]) * operand(inst.operands[1]); break;
            case Opcode::Lt: result = operand(inst.operands[0]) < operand(inst.operands[1]); break;
            default: throw std::runtime_error("unsupported test evaluator opcode");
            }
            if (inst.type.bits < 64) result &= (std::uint64_t{1} << inst.type.bits) - 1;
            values[*inst.result] = result;
        }
        check(jumped, "execution did not reach a terminator");
    }
    throw std::runtime_error("test evaluator exceeded iteration bound");
}
static size_t count(const SimpleSSAGraph& graph, Opcode opcode) {
    size_t result = 0;
    for (auto b : graph.cfg().blocks()) for (const auto& inst : graph.cfg().block(b).instructions)
        result += inst.opcode == opcode;
    return result;
}
static int push_empty(lua_State* L, void*) { lua_newtable(L); return 1; }

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected test fixture directory");
        std::filesystem::path fixtures = argv[1]; std::filesystem::create_directories(fixtures);
        const std::string example = std::string(BLACKSTONE_SOURCE_DIR) + "/kernels/local/strength-redudction.lua";
        check(blackstone_c_api_test(example.c_str()) == 0, "C API smoke test failed");
        blackstone::Runtime runtime; runtime.load_file(example);
        auto metadata = runtime.kernels();
        check(metadata.size() == 1 && metadata[0].requirements.size() == 3 &&
              !metadata[0].documentation.empty(), "incorrect metadata");
        rejects([&] { runtime.load_file(example); }, "duplicate kernel");
        rejects([&] { runtime.load_file((fixtures / "missing.lua").string()); }, "missing.lua");

        for (unsigned bits : {8u, 32u, 64u}) for (int scale : {2, 7, 127}) {
            auto graph = loop_graph(bits, 0, 1, scale); auto original = graph;
            auto result = runtime.run("strength_reduction", graph, blackstone::IntegerSemantics::Modular);
            check(result.changed && !result.invalidated.count("cfg") && result.invalidated.count("def_use"), "analysis contract failed");
            check(graph.verify().empty() && count(graph, Opcode::Mul) == 0 && count(graph, Opcode::Phi) == 3,
                  "strength reduction did not produce valid recurrence");
            for (unsigned n=0; n<=120; ++n) check(evaluate(original, n) == evaluate(graph, n), "semantic mismatch");
            check(!runtime.run("strength_reduction", graph, blackstone::IntegerSemantics::Modular).changed,
                  "repeated execution should reach normal form");
        }
        auto multiple = loop_graph(8, 0, 1, 127, true); auto original_multiple = multiple;
        check(runtime.run("strength_reduction", multiple, blackstone::IntegerSemantics::Modular).changed,
              "multi-backedge loop not reduced");
        for (bool selector : {false, true}) for (unsigned n=0; n<=120; ++n)
            check(evaluate(original_multiple, n, selector) == evaluate(multiple, n, selector), "backedge semantic mismatch");
        for (const auto& parameters : {std::array<int,3>{1,1,7}, {0,2,7}, {0,1,1}, {0,1,-2}}) {
            auto graph = loop_graph(8, parameters[0], parameters[1], parameters[2]); auto size = graph.value_count();
            check(!runtime.run("strength_reduction", graph, blackstone::IntegerSemantics::Modular).changed &&
                  size == graph.value_count(), "unsupported induction was transformed");
        }
        auto unknown = loop_graph();
        check(!runtime.run("strength_reduction", unknown).changed, "unknown overflow semantics authorized rewrite");
        rejects([&] { runtime.run("missing", unknown); }, "unknown kernel");

        auto pattern = blackstone::parse_pattern("(add ?x ?x)");
        check(blackstone::match(pattern, blackstone::Term("add", {{"a"}, {"a"}})).has_value(), "Firestone match failed");
        check(!blackstone::match(pattern, blackstone::Term("add", {{"a"}, {"b"}})), "repeated-variable match accepted unequal values");
        rejects([] { blackstone::parse_pattern("(add (?x a))"); }, "named leaf");

        const std::vector<std::pair<std::string,std::string>> malformed = {
            {"syntax", "lrewrite.kernel.define {"},
            {"missing_metadata", "lrewrite.kernel.run=function() return false end"},
            {"unknown_field", "lrewrite.kernel.define{name='x',wat=1}"},
            {"double_define", "lrewrite.kernel.define{} lrewrite.kernel.define{}"},
            {"wrong_type", "lrewrite.kernel.define{name=1,ir='ssa',phase='p',requires={},preserves={}}"},
            {"bad_list", kernel("x", "return false", "{'a','a'}")},
            {"sparse", kernel("x", "return false", "{[2]='a'}")},
            {"no_run", "lrewrite.kernel.define{name='x',ir='ssa',phase='p',requires={},preserves={}}"}
        };
        for (const auto& fixture : malformed) {
            auto path = fixtures / (fixture.first + ".lua"); write(path, fixture.second);
            rejects([&] { runtime.load_file(path.string()); }, path.string());
            check(runtime.kernels().size() == 1, "invalid load modified catalog");
        }
        write(fixtures / "error.lua", kernel("error", "ctx.blocks[2][1].op='copy'; error('deliberate failure')"));
        write(fixtures / "bad_return.lua", kernel("bad_return", "return 1"));
        write(fixtures / "invalid_ssa.lua", kernel("invalid_ssa", "ctx.blocks[2][1].operands={}; return true"));
        write(fixtures / "missing_analysis.lua", kernel("missing_analysis", "return false", "{'alias_info'}"));
        write(fixtures / "matching.lua", kernel("matching",
            "local m=lrewrite.match('(add ?x ?x)','(add a a)'); assert(m['?x']=='a'); return false"));
        write(fixtures / "metatable.lua", kernel("metatable",
            "ctx.blocks[2][1].op=nil; setmetatable(ctx.blocks[2][1],{__index=function() error('reader metamethod') end}); return true"));
        for (const char* file : {"error", "bad_return", "invalid_ssa", "missing_analysis", "matching", "metatable"})
            runtime.load_file((fixtures / (std::string(file) + ".lua")).string());
        auto unchanged = loop_graph(); auto before = unchanged.value_count();
        rejects([&] { runtime.run("error", unchanged); }, "deliberate failure");
        rejects([&] { runtime.run("bad_return", unchanged); }, "boolean");
        rejects([&] { runtime.run("invalid_ssa", unchanged); }, "invalid rewritten SSA");
        rejects([&] { runtime.run("metatable", unchanged); }, "expected string field 'op'");
        rejects([&] { runtime.run("missing_analysis", unchanged); }, "missing required analysis: alias_info");
        check(unchanged.value_count() == before && count(unchanged, Opcode::Mul) == 1 && unchanged.verify().empty(),
              "failed transformation modified caller graph");
        check(!runtime.run("matching", unchanged).changed, "Lua pattern matcher failed");
        int changed = -1;
        check(blackstone_run(runtime.native_handle(), "matching", "other", nullptr, 0, push_empty, nullptr, &changed) == -1 &&
              std::string(blackstone_last_error(runtime.native_handle())).find("IR") != std::string::npos,
              "wrong host IR accepted");
        check(blackstone_run(runtime.native_handle(), "matching", "ssa", nullptr, 0, push_empty, nullptr, &changed) == 0 && !changed,
              "generic C execution failed");

        auto directory = fixtures / "catalog"; std::filesystem::create_directories(directory);
        write(directory / "b.lua", kernel("b", "assert(private_global==nil); return false"));
        write(directory / "a.lua", "private_global=42\n" + kernel("a", "assert(private_global==42); return false"));
        blackstone::Runtime catalog; catalog.load_directory(directory.string());
        check(catalog.kernels()[0].name == "a" && catalog.kernels()[1].name == "b", "nondeterministic discovery");
        check(!catalog.run("a", unchanged).changed && !catalog.run("b", unchanged).changed, "kernel globals leaked");
        write(directory / "c.lua", "error('broken catalog')");
        blackstone::Runtime atomic;
        rejects([&] { atomic.load_directory(directory.string()); }, "broken catalog");
        check(atomic.kernels().empty(), "directory loading was not atomic");
        std::filesystem::remove_all(fixtures);
        std::cout << "Blackstone runtime, catalog, C API, matching and semantic tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
