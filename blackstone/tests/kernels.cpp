#include "blackstone.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

using namespace firestone::ir;
static void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Case { const char* name; const char* positive; const char* negative; };
// Independently constructed patterns specify the expected applicability of each
// catalog entry. Semantic evaluation below does not use the Lua rule engine.
static const std::array<Case, 70> cases = {{
    {"fold_add", "(add n7 n9)", "(add x n9)"},
    {"fold_sub", "(sub n7 n9)", "(sub x n9)"},
    {"fold_mul", "(mul n7 n9)", "(mul x n9)"},
    {"fold_div", "(div n12 n2)", "(div x n2)"},
    {"fold_mod", "(mod n12 n5)", "(mod x n5)"},
    {"fold_and", "(and n7 n9)", "(and x n9)"},
    {"fold_or", "(or n7 n9)", "(or x n9)"},
    {"fold_xor", "(xor n7 n9)", "(xor x n9)"},
    {"fold_shl", "(shl n7 n2)", "(shl x n2)"},
    {"fold_shr", "(shr n12 n2)", "(shr x n2)"},
    {"fold_neg", "(neg n7)", "(neg x)"},
    {"fold_not", "(not n7)", "(not x)"},
    {"fold_eq", "(eq n7 n9)", "(eq x n9)"},
    {"fold_ne", "(ne n7 n9)", "(ne x n9)"},
    {"fold_lt", "(lt n7 n9)", "(lt x n9)"},
    {"fold_le", "(le n7 n9)", "(le x n9)"},
    {"fold_gt", "(gt n7 n9)", "(gt x n9)"},
    {"fold_ge", "(ge n7 n9)", "(ge x n9)"},
    {"add_zero", "(add x n0)", "(add x n2)"},
    {"sub_zero", "(sub x n0)", "(sub x n2)"},
    {"mul_one", "(mul x n1)", "(mul x n2)"},
    {"mul_zero", "(mul x n0)", "(mul x n2)"},
    {"div_one", "(div x n1)", "(div x n2)"},
    {"mod_one", "(mod x n1)", "(mod x n2)"},
    {"and_zero", "(and x n0)", "(and x n2)"},
    {"and_all_ones", "(and x n-1)", "(and x n2)"},
    {"or_zero", "(or x n0)", "(or x n2)"},
    {"or_all_ones", "(or x n-1)", "(or x n2)"},
    {"xor_zero", "(xor x n0)", "(xor x n2)"},
    {"xor_self", "(xor x x)", "(xor x y)"},
    {"sub_self", "(sub x x)", "(sub x y)"},
    {"and_self", "(and x x)", "(and x y)"},
    {"or_self", "(or x x)", "(or x y)"},
    {"shl_zero", "(shl x n0)", "(shl x n2)"},
    {"shr_zero", "(shr x n0)", "(shr x n2)"},
    {"double_negation", "(neg (neg x))", "(neg x)"},
    {"double_not", "(not (not x))", "(not x)"},
    {"add_negation", "(add x (neg x))", "(add x (neg y))"},
    {"subtract_negation", "(sub x (neg y))", "(sub x y)"},
    {"eq_self", "(eq x x)", "(eq x y)"},
    {"ne_self", "(ne x x)", "(ne x y)"},
    {"lt_self", "(lt x x)", "(lt x y)"},
    {"le_self", "(le x x)", "(le x y)"},
    {"gt_self", "(gt x x)", "(gt x y)"},
    {"ge_self", "(ge x x)", "(ge x y)"},
    {"select_true", "(select b1 x y)", "(select c x y)"},
    {"select_false", "(select b0 x y)", "(select c x y)"},
    {"select_identical", "(select c x x)", "(select c x y)"},
    {"select_boolean_identity", "(select c b1 b0)", "(select c x y)"},
    {"select_boolean_inversion", "(select c b0 b1)", "(select c x y)"},
    {"phi_constant", "@phi_same", "@phi_different"},
    {"copy_propagation", "(add (copy x) y)", "(add x y)"},
    {"constant_propagation", "(add (constant n7) x)", "(add x y)"},
    {"cse_integer_arithmetic", "(add (add x y) (add y x))", "(add (add x y) (sub x y))"},
    {"cse_integer_bitwise", "(xor (and x y) (and y x))", "(xor (and x y) (and x z))"},
    {"cse_integer_comparison", "(xor (eq x y) (eq y x))", "(xor (eq x y) (eq x z))"},
    {"cse_select", "(add (select c x y) (select c x y))", "(add (select c x y) (select c y x))"},
    {"add_associate_constants", "(add (add x n7) n9)", "(add (add x y) n9)"},
    {"mul_associate_constants", "(mul (mul x n7) n9)", "(mul (mul x y) n9)"},
    {"and_associate_constants", "(and (and x n7) n9)", "(and (and x y) n9)"},
    {"or_associate_constants", "(or (or x n7) n9)", "(or (or x y) n9)"},
    {"xor_associate_constants", "(xor (xor x n7) n9)", "(xor (xor x y) n9)"},
    {"and_absorption", "(and x (or x y))", "(and x (or y z))"},
    {"or_absorption", "(or x (and x y))", "(or x (and y z))"},
    {"xor_cancel", "(xor (xor x y) y)", "(xor (xor x y) z)"},
    {"add_sub_cancel", "(sub (add x y) y)", "(sub (add x y) z)"},
    {"sub_add_cancel", "(add (sub x y) y)", "(add (sub x y) z)"},
    {"sub_sub_cancel", "(sub x (sub x y))", "(sub x (sub z y))"},
    {"and_complement", "(and x (not x))", "(and x (not y))"},
    {"or_complement", "(or x (not x))", "(or x (not y))"}
}};

static const std::map<std::string, Opcode> opcodes = {
    {"constant",Opcode::Constant}, {"copy",Opcode::Copy}, {"add",Opcode::Add},
    {"sub",Opcode::Sub}, {"mul",Opcode::Mul}, {"div",Opcode::Div}, {"mod",Opcode::Mod},
    {"and",Opcode::And}, {"or",Opcode::Or}, {"xor",Opcode::Xor}, {"shl",Opcode::Shl},
    {"shr",Opcode::Shr}, {"neg",Opcode::Neg}, {"not",Opcode::Not}, {"eq",Opcode::Eq},
    {"ne",Opcode::Ne}, {"lt",Opcode::Lt}, {"le",Opcode::Le}, {"gt",Opcode::Gt},
    {"ge",Opcode::Ge}, {"select",Opcode::Select}
};
struct Builder {
    SimpleSSAGraph graph;
    Type numeric;
    std::map<std::string, ValueId> parameters;
    explicit Builder(Type type) : numeric(std::move(type)) {
        graph.cfg().create_block("entry");
        for (const char* p : {"x", "y", "z"}) parameters[p] = graph.parameter(numeric);
        parameters["c"] = graph.parameter(Type::boolean());
    }
    Operand expression(const blackstone::Term& term, BlockId block) {
        if (term.children.empty()) {
            auto p = parameters.find(term.op);
            if (p != parameters.end()) return Operand::ref(p->second);
            if (term.op == "b0" || term.op == "b1") return Operand::constant(term.op == "b1", Type::boolean());
            check(term.op.size()>1 && term.op[0]=='n', "invalid test literal");
            auto n = std::stoll(term.op.substr(1));
            return numeric.kind == TypeKind::Float ? Operand::real(static_cast<double>(n), numeric) : Operand::constant(n, numeric);
        }
        std::vector<Operand> operands;
        for (const auto& child : term.children) operands.push_back(expression(child, block));
        auto opcode = opcodes.at(term.op);
        auto type = operand_type(operands[opcode == Opcode::Select ? 1 : 0], [&](ValueId id) { return graph.type(id); });
        if (is_comparison(opcode)) type = Type::boolean();
        return Operand::ref(graph.define(block, opcode, type, std::move(operands)));
    }
};
static SimpleSSAGraph make_graph(const std::string& source, Type type=Type::integer(8)) {
    Builder builder(type); auto& graph = builder.graph; auto& cfg = graph.cfg();
    if (source[0] == '@') {
        auto left=cfg.create_block("left"), right=cfg.create_block("right"), join=cfg.create_block("join");
        cfg.branch(0, Operand::ref(builder.parameters["c"]), left, right);
        cfg.jump(left, join); cfg.jump(right, join);
        auto root=graph.phi(join, type, {{left,Operand::constant(7,type)},
                                      {right,Operand::constant(source=="@phi_same" ? 7 : 9,type)}});
        // Leave another phi in place to exercise the required phi-prefix order.
        graph.phi(join, type, {{left,Operand::constant(9,type)}, {right,Operand::constant(10,type)}});
        cfg.return_value(join, Operand::ref(root));
    } else cfg.return_value(0, builder.expression(firestone::rewrite::parse(source), 0));
    auto errors=graph.verify();
    check(errors.empty(), "invalid fixture: " + source + (errors.empty() ? "" : ": " + errors.front().message));
    return std::move(graph);
}
static std::uint64_t mask(unsigned bits) {
    return bits == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << bits)-1;
}
static std::int64_t signed_value(std::uint64_t n, unsigned bits) {
    n &= mask(bits);
    if (bits < 64 && (n & (std::uint64_t{1} << (bits-1)))) n |= ~mask(bits);
    std::int64_t result; std::memcpy(&result, &n, sizeof(result)); return result;
}
// Modular evaluation uses unsigned arithmetic to avoid host signed overflow.
// Run both signed and unsigned division/ordering/right-shift interpretations;
// applicable rules must agree without relying on unencoded signedness.
static std::uint64_t evaluate(const SimpleSSAGraph& graph, const std::array<std::uint64_t,4>& args, bool signed_mode) {
    std::map<ValueId,std::uint64_t> values;
    for (ValueId id=0; id<4; ++id) values[id] = args[id] & mask(graph.type(id).bits);
    auto type_of = [&](const Operand& o) { return operand_type(o, [&](ValueId id) { return graph.type(id); }); };
    auto operand = [&](const Operand& o) {
        return (o.kind == OperandKind::Value ? values.at(o.value) : static_cast<std::uint64_t>(o.integer)) & mask(type_of(o).bits);
    };
    auto block=graph.cfg().entry(), previous=invalid_id;
    for (unsigned steps=0; steps<100; ++steps) {
        const auto& instructions=graph.cfg().block(block).instructions;
        auto pending=values;
        for (const auto& i : instructions) {
            if (i.opcode != Opcode::Phi) break;
            auto p=std::find(i.incoming_blocks.begin(), i.incoming_blocks.end(), previous);
            check(p!=i.incoming_blocks.end(), "phi evaluator has no incoming edge");
            pending[*i.result]=operand(i.operands[p-i.incoming_blocks.begin()]);
        }
        values=std::move(pending);
        for (const auto& i : instructions) {
            if (i.opcode==Opcode::Phi) continue;
            if (i.opcode==Opcode::Return) return operand(i.operands[0]);
            if (i.opcode==Opcode::Jump || i.opcode==Opcode::Branch) {
                previous=block; block=i.targets[i.opcode==Opcode::Jump || operand(i.operands[0]) ? 0 : 1]; break;
            }
            const auto a=operand(i.operands[0]);
            const auto b=i.operands.size()>1 ? operand(i.operands[1]) : a;
            const auto width=type_of(i.operands[0]).bits;
            const auto x=signed_value(a,width), y=signed_value(b,width);
            std::uint64_t r=0;
            switch (i.opcode) {
            case Opcode::Constant: case Opcode::Copy: r=a; break;
            case Opcode::Add: r=a+b; break;
            case Opcode::Sub: r=a-b; break;
            case Opcode::Mul: r=a*b; break;
            case Opcode::Div: check(b!=0, "test division by zero"); r=signed_mode ? static_cast<std::uint64_t>(x/y) : a/b; break;
            case Opcode::Mod: check(b!=0, "test remainder by zero"); r=signed_mode ? static_cast<std::uint64_t>(x%y) : a%b; break;
            case Opcode::And: r=a&b; break;
            case Opcode::Or: r=a|b; break;
            case Opcode::Xor: r=a^b; break;
            case Opcode::Shl: check(b<width, "test invalid shift"); r=a<<b; break;
            case Opcode::Shr: check(b<width, "test invalid shift"); r=signed_mode ? static_cast<std::uint64_t>(x>>b) : a>>b; break;
            case Opcode::Neg: r=std::uint64_t{0}-a; break;
            case Opcode::Not: r=~a; break;
            case Opcode::Eq: r=a==b; break;
            case Opcode::Ne: r=a!=b; break;
            case Opcode::Lt: r=signed_mode ? x<y : a<b; break;
            case Opcode::Le: r=signed_mode ? x<=y : a<=b; break;
            case Opcode::Gt: r=signed_mode ? x>y : a>b; break;
            case Opcode::Ge: r=signed_mode ? x>=y : a>=b; break;
            case Opcode::Select: r=a ? b : operand(i.operands[2]); break;
            default: throw std::runtime_error("unsupported evaluator operation");
            }
            values[*i.result]=r & mask(i.type.bits);
        }
    }
    throw std::runtime_error("test execution exceeded CFG limit");
}
static std::string fingerprint(const SimpleSSAGraph& graph) {
    std::string result;
    for (auto b : graph.cfg().blocks()) for (const auto& i : graph.cfg().block(b).instructions) {
        result+=std::to_string(b)+":"+std::to_string(static_cast<int>(i.opcode))+":";
        for (const auto& o : i.operands) result+=std::to_string(static_cast<int>(o.kind))+":"+
            std::to_string(o.value)+":"+std::to_string(o.integer)+",";
        result+=";";
    }
    return result;
}
static void equivalent(const SimpleSSAGraph& original, const SimpleSSAGraph& rewritten, const std::string& label) {
    const std::array<std::uint64_t,8> edges = {0,1,2,127,128,255,0x8000000000000000ULL,0xffffffffffffffffULL};
    for (auto x : edges) for (auto y : edges) for (bool condition : {false,true}) for (bool signed_mode : {false,true}) {
        std::array<std::uint64_t,4> input{x,y,x^y,condition};
        check(evaluate(original,input,signed_mode)==evaluate(rewritten,input,signed_mode), "semantic mismatch: "+label);
    }
    std::uint64_t state=0x8fe16251ab9cd307ULL;
    for (unsigned n=0; n<64; ++n) {
        std::array<std::uint64_t,4> input;
        for (auto& v : input) { state^=state<<13; state^=state>>7; state^=state<<17; v=state; }
        input[3]&=1;
        for (bool signed_mode : {false,true})
            check(evaluate(original,input,signed_mode)==evaluate(rewritten,input,signed_mode), "random semantic mismatch: "+label);
    }
}
static void positive(blackstone::Runtime& runtime, const std::string& name, const std::string& source, unsigned bits) {
    auto graph=make_graph(source,Type::integer(bits)), original=graph;
    auto result=runtime.run(name,graph,blackstone::IntegerSemantics::Modular);
    check(result.changed && fingerprint(graph)!=fingerprint(original), "positive pattern did not change: "+name);
    check(graph.verify().empty(), "invalid rewritten SSA: "+name);
    check(!result.invalidated.count("cfg") && !result.invalidated.count("dominators") && result.invalidated.count("def_use"),
          "incorrect analysis preservation: "+name);
    equivalent(original,graph,name+"/"+std::to_string(bits));
}
static void negative(blackstone::Runtime& runtime, const std::string& name, const std::string& source) {
    auto graph=make_graph(source); auto before=fingerprint(graph);
    check(!runtime.run(name,graph,blackstone::IntegerSemantics::Modular).changed && fingerprint(graph)==before,
          "negative pattern was transformed: "+name+" "+source);
}
static SimpleSSAGraph diamond(const std::string& source, bool dominating) {
    Builder builder(Type::integer(8)); auto& graph=builder.graph; auto& cfg=graph.cfg();
    auto left=cfg.create_block("left"), right=cfg.create_block("right"), join=cfg.create_block("join");
    if (dominating) builder.expression(firestone::rewrite::parse(source),0);
    cfg.branch(0,Operand::ref(builder.parameters["c"]),left,right);
    auto a=builder.expression(firestone::rewrite::parse(source),left);
    auto b=builder.expression(firestone::rewrite::parse(source),right);
    cfg.jump(left,join); cfg.jump(right,join);
    auto root=graph.phi(join,graph.type(a.value),{{left,a},{right,b}});
    cfg.return_value(join,Operand::ref(root));
    check(graph.verify().empty(),"invalid CSE diamond"); return std::move(graph);
}

int main() {
    try {
        blackstone::Runtime runtime;
        runtime.load_directory(std::string(BLACKSTONE_SOURCE_DIR)+"/kernels");
        auto catalog=runtime.kernels();
        check(catalog.size()==71,"expected exactly 70 additional kernels plus strength reduction");
        std::set<std::string> names;
        for (const auto& k : catalog) {
            check(names.insert(k.name).second && !k.documentation.empty() && k.ir=="ssa", "invalid catalog metadata");
        }
        for (const auto& test : cases) {
            check(names.erase(test.name)==1,"unregistered test kernel: "+std::string(test.name));
            for (unsigned bits : {8u,32u,64u}) positive(runtime,test.name,test.positive,bits);
            negative(runtime,test.name,test.negative);
            auto unknown=make_graph(test.positive); auto before=fingerprint(unknown);
            check(!runtime.run(test.name,unknown).changed && before==fingerprint(unknown),
                  "unknown semantics authorized rewrite: "+std::string(test.name));
        }
        check(names==std::set<std::string>{"strength_reduction"},"catalog entry has no semantic test");

        // Overflow, sign bits, shift bounds and signedness restrictions.
        for (const auto& test : std::vector<Case>{
                {"fold_add","(add n127 n1)",""}, {"fold_mul","(mul n127 n127)",""},
                {"fold_neg","(neg n-128)",""}, {"fold_not","(not n-1)",""},
                {"fold_eq","(eq n-1 n255)",""}, {"fold_ne","(ne n-1 n255)",""},
                {"fold_shl","(shl n127 n7)",""}, {"fold_div","(div n0 n7)",""},
                {"fold_mod","(mod n0 n7)",""}}) positive(runtime,test.name,test.positive,8);
        positive(runtime,"fold_add","(add n9223372036854775807 n1)",64);
        positive(runtime,"fold_neg","(neg n-9223372036854775808)",64);
        positive(runtime,"fold_mul","(mul n9223372036854775807 n2)",64);
        for (const auto& test : std::vector<Case>{
                {"fold_div","","(div n7 n0)"}, {"fold_div","","(div n-128 n-1)"},
                {"fold_mod","","(mod n7 n0)"}, {"fold_mod","","(mod n-7 n2)"},
                {"fold_shl","","(shl n7 n8)"}, {"fold_shl","","(shl n7 n-1)"},
                {"fold_shr","","(shr n-1 n2)"}, {"fold_shr","","(shr n7 n8)"},
                {"fold_lt","","(lt n-1 n1)"}, {"fold_le","","(le n-1 n1)"},
                {"fold_gt","","(gt n-1 n1)"}, {"fold_ge","","(ge n-1 n1)"}})
            negative(runtime,test.name,test.negative);

        // Commuted matching and constants reached through definitions.
        for (const auto& test : std::vector<Case>{
                {"add_zero","(add n0 x)",""}, {"mul_one","(mul n1 x)",""},
                {"mul_zero","(mul n0 x)",""}, {"and_all_ones","(and n-1 x)",""},
                {"or_all_ones","(or n-1 x)",""}, {"and_zero","(and n0 x)",""},
                {"or_zero","(or n0 x)",""}, {"xor_zero","(xor n0 x)",""},
                {"add_negation","(add (neg x) x)",""},
                {"and_absorption","(and (or y x) x)",""},
                {"or_absorption","(or (and y x) x)",""},
                {"xor_cancel","(xor y (xor y x))",""},
                {"sub_add_cancel","(add y (sub x y))",""},
                {"add_sub_cancel","(sub (add y x) y)",""},
                {"and_complement","(and (not x) x)",""},
                {"or_complement","(or (not x) x)",""},
                {"fold_add","(add (copy (constant n7)) n9)",""},
                {"add_zero","(add x (copy (constant n0)))",""},
                {"add_associate_constants","(add n9 (add n7 x))",""}})
            positive(runtime,test.name,test.positive,8);

        for (const auto& test : std::vector<Case>{
                {"cse_integer_arithmetic","(add x y)",""},
                {"cse_integer_bitwise","(and x y)",""},
                {"cse_integer_comparison","(eq x y)",""},
                {"cse_select","(select c x y)",""}}) {
            auto disjoint=diamond(test.positive,false), original=disjoint;
            check(!runtime.run(test.name,disjoint,blackstone::IntegerSemantics::Modular).changed &&
                  fingerprint(original)==fingerprint(disjoint),"CSE ignored dominance: "+std::string(test.name));
            auto graph=diamond(test.positive,true), before=graph;
            check(runtime.run(test.name,graph,blackstone::IntegerSemantics::Modular).changed,
                  "cross-block CSE failed: "+std::string(test.name));
            equivalent(before,graph,test.name);
        }
        // Ordered comparisons may not be commuted by CSE.
        negative(runtime,"cse_integer_comparison","(xor (lt x y) (lt y x))");
        for (const auto& test : cases) {
            auto floats=make_graph("(add x y)",Type::floating(64));
            check(!runtime.run(test.name,floats,blackstone::IntegerSemantics::Modular).changed,
                  "integer kernel rewrote floats: "+std::string(test.name));
            Type named=Type::integer(8); named.name="custom";
            auto custom=make_graph("(add x n0)",named);
            check(!runtime.run(test.name,custom,blackstone::IntegerSemantics::Modular).changed,
                  "integer kernel rewrote named type: "+std::string(test.name));
        }
        for (const char* name : {"eq_self","ne_self","lt_self","le_self","gt_self","ge_self"}) {
            std::string op=std::string(name).substr(0,2);
            auto floats=make_graph("("+op+" x x)",Type::floating(64));
            check(!runtime.run(name,floats,blackstone::IntegerSemantics::Modular).changed,"NaN-sensitive identity was applied");
        }
        // Eliminating an arithmetic result must retain effectful input evaluation.
        SimpleSSAGraph effects; auto block=effects.cfg().create_block();
        auto address=effects.parameter(Type::pointer()); auto integer=Type::integer(8);
        auto load=effects.define(block,Opcode::Load,integer,{Operand::ref(address)});
        auto product=effects.define(block,Opcode::Mul,integer,{Operand::ref(load),Operand::constant(0,integer)});
        effects.cfg().return_value(block,Operand::ref(product));
        check(runtime.run("mul_zero",effects,blackstone::IntegerSemantics::Modular).changed &&
              effects.cfg().block(block).instructions[0].opcode==Opcode::Load,
              "zero multiplication removed observable operand evaluation");
        std::cout << "All 70 additional kernels passed positive, negative, SSA and signed/unsigned differential tests\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
