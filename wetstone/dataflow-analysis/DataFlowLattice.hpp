#pragma once
#include "../utils/Graph.hpp"
#include "../abstract-interp/AbstractDomain.hpp"
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
namespace wetstone::df {
enum class Direction{Forward,Backward};
template<class State> struct Result {
    std::map<ir::BlockId,State> in,out;
    std::size_t evaluations=0;bool converged=true;
};
// Transfer maps the input side of a block to its output side.
// All reachable blocks are initially bottom; boundary facts are joined.
template<ai::AbstractDomain State,class Transfer>
Result<State> solve(const ir::Function& f,Transfer transfer,
      std::map<ir::BlockId,State> boundary={},Direction direction=Direction::Forward,
      std::size_t limit=100000,std::size_t widening_delay=3,std::size_t narrowing_iterations=0) {
    f.validate();auto reach=graph::reachable(f);
    for(const auto& [b,_]:boundary)if(!reach.count(b))throw std::invalid_argument("boundary outside reachable CFG");
    Result<State> r;for(auto b:reach){r.in[b]=State::bottom();r.out[b]=State::bottom();}
    auto order=graph::reverse_postorder(f);if(direction==Direction::Backward)std::reverse(order.begin(),order.end());
    std::queue<ir::BlockId> work;std::set<ir::BlockId> queued;
    for(auto b:order){work.push(b);queued.insert(b);}
    std::map<ir::BlockId,std::size_t> visits;
    while(!work.empty()) {
        if(r.evaluations>=limit){r.converged=false;break;}
        const auto b=work.front();work.pop();queued.erase(b);++r.evaluations;
        State input=State::bottom();
        if(auto it=boundary.find(b);it!=boundary.end())input=it->second;
        const auto neighbors=direction==Direction::Forward?f.predecessors(b):f.successors(b);
        for(auto p:neighbors)if(reach.count(p))input=input.join(direction==Direction::Forward?r.out.at(p):r.in.at(p));
        State& old_input=direction==Direction::Forward?r.in[b]:r.out[b];
        if(visits[b]++>=widening_delay)input=old_input.widen(input);
        State output=transfer(b,input);
        State& old_output=direction==Direction::Forward?r.out[b]:r.in[b];
        bool changed=!(old_output==output);
        old_input=std::move(input);
        old_output=std::move(output);
        if(changed){auto next=direction==Direction::Forward?f.successors(b):f.predecessors(b);
            for(auto n:next)if(reach.count(n)&&queued.insert(n).second)work.push(n);}
    }
    // Narrowing pass: refine the widened post-fixpoint toward the true fixpoint.
    // Only domains exposing a dedicated narrow() participate; the generic
    // ai::narrow (which is just meet) is deliberately NOT used here, because meet
    // can drop below the fixpoint and forge an unsound proof. Narrowing never
    // widens, so it is monotone and bounded by narrowing_iterations.
    if constexpr(requires(const State& a,const State& b){ a.narrow(b); }) {
        if(narrowing_iterations>0&&r.converged) {
            for(std::size_t pass=0;pass<narrowing_iterations;++pass) {
                bool changed=false;
                for(auto b:order) {
                    if(!reach.count(b))continue;
                    if(r.evaluations>=limit){r.converged=false;break;}
                    ++r.evaluations;
                    State input=State::bottom();
                    if(auto it=boundary.find(b);it!=boundary.end())input=it->second;
                    const auto neighbors=direction==Direction::Forward?f.predecessors(b):f.successors(b);
                    for(auto p:neighbors)if(reach.count(p))input=input.join(direction==Direction::Forward?r.out.at(p):r.in.at(p));
                    State& old_input=direction==Direction::Forward?r.in[b]:r.out[b];
                    input=old_input.narrow(input);
                    State output=transfer(b,input);
                    State& old_output=direction==Direction::Forward?r.out[b]:r.in[b];
                    bool ch=!(old_output==output);
                    old_input=std::move(input);
                    old_output=std::move(output);
                    if(ch)changed=true;
                }
                if(!changed||!r.converged)break;
            }
        }
    }
    return r;
}
}
