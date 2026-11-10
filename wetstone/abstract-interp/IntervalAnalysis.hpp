#pragma once
#include "Transfer.hpp"
#include "../utils/Graph.hpp"
#include "../utils/Budget.hpp"
#include <queue>
namespace wetstone::ai {
struct IntervalAnalysisResult {
    std::map<ir::BlockId,Environment> in,out;
    std::size_t evaluations=0;bool converged=true;
};
inline IntervalAnalysisResult analyze_intervals(const ir::Function& f,std::size_t limit=100000,std::size_t widening_delay=2,std::size_t narrowing_iterations=0){
    f.validate();auto reachable=graph::reachable(f);
    IntervalAnalysisResult r;
    std::map<ir::BlockId,std::size_t> visits;
    for(auto id:reachable){r.in[id]=Environment::bottom();r.out[id]=Environment::bottom();}
    std::queue<ir::BlockId> q;q.push(f.entry);std::set<ir::BlockId> enqueued{f.entry};
    while(!q.empty()){
        if(r.evaluations>=limit){r.converged=false;break;}
        auto id=q.front();q.pop();enqueued.erase(id);++r.evaluations;
        Environment next=(id==f.entry)?Environment::top():Environment::bottom();
        for(auto p:f.predecessors(id))if(reachable.count(p))next=next.join(r.out.at(p));
        // A backedge may create an unbounded ascending chain.
        if(visits[id]++>=widening_delay)next=r.in[id].widen(next);
        r.in[id]=next;
        auto outgoing=transfer_block(f.block(id),next);
        if(!(outgoing==r.out[id])){
            r.out[id]=outgoing;
            for(auto s:f.successors(id))if(reachable.count(s)&&enqueued.insert(s).second)q.push(s);
        }
    }
    // Narrowing: retract widened +/-infinity endpoints toward the joined successor
    // value. Environment::narrow only tightens and never goes below the join, so
    // feasible states are preserved. Bounded by narrowing_iterations.
    if(narrowing_iterations>0&&r.converged){
        auto order=graph::reverse_postorder(f);
        for(std::size_t pass=0;pass<narrowing_iterations;++pass){
            bool changed=false;
            for(auto id:order){
                if(!reachable.count(id))continue;
                if(r.evaluations>=limit){r.converged=false;break;}
                ++r.evaluations;
                Environment next=(id==f.entry)?Environment::top():Environment::bottom();
                for(auto p:f.predecessors(id))if(reachable.count(p))next=next.join(r.out.at(p));
                next=r.in[id].narrow(next);
                r.in[id]=next;
                auto outgoing=transfer_block(f.block(id),next);
                if(!(outgoing==r.out[id])){r.out[id]=outgoing;changed=true;}
            }
            if(!changed||!r.converged)break;
        }
    }
    return r;
}
}
