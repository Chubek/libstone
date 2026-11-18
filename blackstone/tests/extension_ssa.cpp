/* Exercise representative new kernels through the real Firestone SSA adapter. */
#include "blackstone.hpp"
#include <libir/SimpleSSAGraph.hpp>
#include <stdexcept>
#include <iostream>
#include <string>
using namespace firestone::ir;
static void ensure(bool test,const std::string& what) {
    if(!test)throw std::runtime_error(what);
}
static SimpleSSAGraph start() {
    SimpleSSAGraph g;
    g.cfg().create_block("entry");
    return g;
}
static void check(const char* name,SimpleSSAGraph& graph,blackstone::Runtime& runtime){
    ensure(graph.verify().empty(),std::string(name)+" invalid input SSA");
    auto r=runtime.run(name,graph,blackstone::IntegerSemantics::Modular);
    ensure(r.changed,std::string(name)+" failed positive rewrite");
    ensure(graph.verify().empty(),std::string(name)+" invalid output SSA");
    ensure(r.invalidated.count("def_use") != 0,std::string(name)+" missing invalidation");
}
int main(int argc,char** argv){
 try {
    ensure(argc==2,"expected kernel directory");
    blackstone::Runtime runtime;
    runtime.load_directory(argv[1]);
    ensure(runtime.kernels().size()==191,"catalog must contain 191 kernels");
    const auto t=Type::integer(8);
    {
        auto g=start();auto x=g.parameter(t);
        auto r=g.define(0,Opcode::Sub,t,{Operand::constant(0,t),Operand::ref(x)});
        g.cfg().return_value(0,Operand::ref(r));
        check("ext_zero_minus_x",g,runtime);
    }
    {
        auto g=start();auto x=g.parameter(t);
        auto r=g.define(0,Opcode::Mul,t,{Operand::ref(x),Operand::constant(4,t)});
        g.cfg().return_value(0,Operand::ref(r));
        check("ext_multiply_by_power_of_two",g,runtime);
    }
    {
        auto g=start();auto x=g.parameter(t),y=g.parameter(t),z=g.parameter(t);
        auto a=g.define(0,Opcode::And,t,{Operand::ref(y),Operand::ref(x)});
        auto b=g.define(0,Opcode::And,t,{Operand::ref(z),Operand::ref(x)});
        auto r=g.define(0,Opcode::Or,t,{Operand::ref(a),Operand::ref(b)});
        g.cfg().return_value(0,Operand::ref(r));
        check("ext_factor_bitwise_or_of_ands",g,runtime);
        const auto& instructions=g.cfg().block(0).instructions;
        ensure(instructions.size()==5 && instructions[2].opcode==Opcode::Or &&
               instructions[3].opcode==Opcode::And && instructions[3].result==r,
               "factoring must allocate one temporary before the retained original result");
    }
    {
        auto g=start();const auto boolean=Type::boolean();
        auto x=g.parameter(boolean),y=g.parameter(boolean),z=g.parameter(boolean);
        auto a=g.define(0,Opcode::And,boolean,{Operand::ref(x),Operand::ref(y)});
        auto b=g.define(0,Opcode::And,boolean,{Operand::ref(x),Operand::ref(z)});
        auto r=g.define(0,Opcode::Or,boolean,{Operand::ref(a),Operand::ref(b)});
        g.cfg().return_value(0,Operand::ref(r));
        const auto values=g.value_count();
        ensure(!runtime.run("ext_factor_bitwise_or_of_ands",g,blackstone::IntegerSemantics::Modular).changed &&
               g.value_count()==values && g.cfg().block(0).instructions[2].opcode==Opcode::Or &&
               g.verify().empty(),"factoring must skip unsupported one-bit temporary allocation");
    }
    {
        auto g=start();auto x=g.parameter(t),y=g.parameter(t);
        auto a=g.define(0,Opcode::Mul,t,{Operand::ref(x),Operand::constant(3,t)});
        auto b=g.define(0,Opcode::Mul,t,{Operand::ref(y),Operand::constant(3,t)});
        auto r=g.define(0,Opcode::Eq,Type::boolean(),{Operand::ref(a),Operand::ref(b)});
        g.cfg().return_value(0,Operand::ref(r));
        check("ext_eq_cancel_mul_constant",g,runtime);
    }
    {
        auto g=start();auto x=g.parameter(t),y=g.parameter(t);
        auto cond=g.define(0,Opcode::Eq,Type::boolean(),{Operand::ref(x),Operand::ref(y)});
        auto r=g.define(0,Opcode::Select,t,{Operand::ref(cond),Operand::ref(x),Operand::ref(y)});
        g.cfg().return_value(0,Operand::ref(r));
        check("ext_select_equal_choose_right",g,runtime);
    }
    {
        auto g=start();auto x=g.parameter(t),y=g.parameter(t);
        auto a=g.define(0,Opcode::Shr,t,{Operand::ref(x),Operand::constant(2,t)});
        auto b=g.define(0,Opcode::Shr,t,{Operand::ref(y),Operand::constant(2,t)});
        auto r=g.define(0,Opcode::Or,t,{Operand::ref(a),Operand::ref(b)});
        g.cfg().return_value(0,Operand::ref(r));
        check("ext_distribute_shr_or",g,runtime);
    }
    std::cout<<"Six new kernels passed native Firestone SSA smoke tests\n";
    return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
