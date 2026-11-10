#include "Wetstone.hpp"
#include <cassert>
#include <iostream>
using namespace wetstone;
static ir::Function make(const char* name,const char* callee="") {
    ir::Function f;f.name=name;f.entry=0;
    ir::Block b;b.id=0;
    if(*callee)b.instructions.push_back({ir::Opcode::Call,{}, {}, {}, {},callee});
    else b.instructions.push_back({ir::Opcode::Load,1,{ir::Operand::constant(0)}});
    b.instructions.push_back({ir::Opcode::Return});f.blocks.push_back(b);return f;
}
int main(){
    ir::Program p;p.functions={make("a","b"),make("b","a"),make("c")};p.validate();
    auto cg=interprocedural::call_graph(p);assert(cg.unresolved.empty());
    auto scc=interprocedural::recursive_components(cg);
    bool found=false;for(auto& c:scc)if(c.size()==2&&c.count("a")&&c.count("b"))found=true;
    assert(found);
    auto effects=interprocedural::effect_summaries(p);
    assert(effects.at("a").reads&&effects.at("a").writes);
    assert(effects.at("c").reads&&!effects.at("c").writes);
    auto models=models::Registry::portable_defaults();assert(models.lookup("memcpy"));
    AnalysisManager manager;auto pipeline=verification::standard_suite();
    auto ds=pipeline.analyze(p.functions.at(2),manager);assert(ds.empty());
    auto cache=manager.get<int>("x",p.functions[0],0,[]{return 20;});assert(cache==20);
    auto cache2=manager.get<int>("x",p.functions[0],1,[]{return 30;});assert(cache2==30);
    interprocedural::CallString cs;cs.k=2;
    cs=cs.push("a").push("b").push("c");assert(cs.sites.size()==2&&cs.sites[0]=="b");
    std::cout<<"Interprocedural tests passed\n";
}
