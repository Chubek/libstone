#include "limeburg/target.hpp"
#include "../../tests/test.hpp"
#include <climits>

namespace {
using namespace limestone;
using Dependency = schedrow::Dependency;

void ordered_covers() {
  // Source order is acyclic: 10 feeds 40, and the explicit path is
  // 10 -> 20 -> 30 -> 40. Contracting 10 and 40 would violate that path.
  unisel::Program program{{
    {10,"seed",{},{},"i64"}, {20,"step",{},{},"i64"},
    {30,"step",{},{},"i64"}, {40,"finish",{10},{},"i64"}
  },{40},{{10,20,schedrow::DepKind::Ordering,2},
          {20,30,schedrow::DepKind::Ordering,1},
          {30,40,schedrow::DepKind::Ordering,3,0,true}}};
  unisel::Pattern fused{4,"fused","finish","FUSED",{},0};
  fused.tree=unisel::PatternTree{"finish","","i64",{{"seed","","i64"}}};
  std::vector<unisel::Pattern> patterns{
    {1,"seed","seed","SEED",{},3}, {2,"step","step","STEP",{},5},
    {3,"finish","finish","FINISH",{"i64"},9}, fused
  };
  CHECK(take(unisel::build_model(program,patterns)).candidates.size()==5);
  auto best=take(unisel::solve(program,patterns));
  CHECK(best.cost==22&&best.selected.size()==4&&best.selected.back().pattern==3);
  auto scheduler=take(unisel::emit_scheduler(program,patterns,best));
  CHECK(scheduler.instructions.size()==4&&scheduler.instructions.back().opcode=="FINISH");
  auto has_edge=[](const auto& region, const Dependency& expected) {
    return std::any_of(region.deps.begin(),region.deps.end(),[&](const auto& d){
      return d.producer==expected.producer&&d.consumer==expected.consumer&&
             d.kind==expected.kind&&d.latency==expected.latency&&d.scheduler_only==expected.scheduler_only;
    });
  };
  for(const auto& dep:program.dependencies)CHECK(has_edge(scheduler,dep));
  take(schedrow::verify(scheduler,{},take(schedrow::schedule(scheduler,{}))));
  fails(unisel::solve(program,{patterns[1],fused}),Error::Code::Unsatisfiable);
  fails(unisel::solve_greedy(program,patterns),Error::Code::Conflict);

  // Without the last edge, the fusion becomes schedulable; the first ordering
  // edge now originates at the fused instruction and keeps its latency.
  program.dependencies.pop_back();
  auto fused_solution=take(unisel::solve(program,patterns));
  CHECK(fused_solution.cost==10&&fused_solution.selected.size()==3);
  auto fused_scheduler=take(unisel::emit_scheduler(program,patterns,fused_solution));
  CHECK(fused_scheduler.instructions.front().opcode=="FUSED");
  CHECK(has_edge(fused_scheduler,{40,20,schedrow::DepKind::Ordering,2}));
  take(schedrow::verify(fused_scheduler,{},take(schedrow::schedule(fused_scheduler,{}))));
  program.nodes[0].may_trap=true;
  patterns[0].supports_side_effects=true;
  patterns[3].supports_side_effects=true;
  CHECK(take(unisel::solve(program,patterns)).cost==22);
}

void objective() {
  unisel::Program program{{{1,"a",{},{},"i64"}, {2,"b",{1},{},"i64"}},{2}};
  unisel::Pattern fused{4,"ab","b","AB",{},INT_MAX};
  fused.tree=unisel::PatternTree{"b","","i64",{{"a","","i64"}}};
  std::vector<unisel::Pattern> patterns{
    {1,"a","a","A",{},INT_MAX},{2,"b","b","B",{"i64"},INT_MAX},
    {3,"b-tie","b","B2",{"i64"},INT_MAX},fused
  };
  auto selected=take(unisel::solve(program,patterns));
  CHECK(selected.cost==INT_MAX&&selected.selected.size()==1&&selected.selected[0].pattern==4);
  fails(unisel::solve(program,{patterns[0],patterns[1]}),Error::Code::ResourceLimit);
  CHECK(take(unisel::solve({},{})).selected.empty());

  // Compare all small weighted selections to exhaustive legal covers,
  // including all-zero cost, equal costs, and inverted rule declaration order.
  for(int a:{0,3,7})for(int b:{0,2,9})for(int ab:{0,5,16}) {
    patterns[0].cost=a;
    patterns[1].cost=patterns[2].cost=b;
    patterns[3].cost=ab;
    auto candidates=take(unisel::build_model(program,patterns)).candidates;
    std::optional<std::pair<int,unsigned>> optimum;
    for(unsigned mask=1;mask<(1u<<candidates.size());++mask) {
      unisel::Selection probe;
      for(size_t i=0;i<candidates.size();++i)if(mask&(1u<<(candidates.size()-1-i))) {
        probe.selected.push_back(candidates[i]);
        probe.cost+=candidates[i].cost;
      }
      if(!unisel::emit_scheduler(program,patterns,probe))continue;
      if(!optimum||probe.cost<optimum->first||
          (probe.cost==optimum->first&&mask>optimum->second))optimum={{probe.cost,mask}};
    }
    CHECK(optimum.has_value());
    std::reverse(patterns.begin(),patterns.end());
    auto solved=take(unisel::solve(program,patterns));
    unsigned mask=0;
    for(const auto& candidate:solved.selected) {
      auto it=std::find_if(candidates.begin(),candidates.end(),[&](const auto& c){
        return c.root==candidate.root&&c.pattern==candidate.pattern;
      });
      CHECK(it!=candidates.end());
      mask|=1u<<(candidates.end()-it-1);
    }
    CHECK((std::pair{solved.cost,mask}==*optimum));
    std::reverse(patterns.begin(),patterns.end());
  }
}

void shared_boundaries() {
  limeburg::RuleSet rules{{
    {1,"lhs","input","lhs",{},2,""},
    {2,"rhs","input","rhs",{},4,""},
    {3,"value","mix","value",{"lhs","rhs"},3,"MIX"},
    {4,"value","mix","value",{"lhs","rhs","rhs"},1,"WRONG_ARITY"},
    {5,"value","other","value",{"lhs","rhs"},1,"WRONG_OPCODE"}
  },{{"lhs",15},{"rhs",7},{"value",0}}};
  std::vector<limeburg::Node> nodes{
    {1,"input","i64",{},0,false,false}, {2,"mix","i64",{1,1}}
  };
  nodes[0].register_class="G";
  auto analyzed=take(limeburg::analyze(nodes,2,rules,true));
  CHECK(analyzed.attempts.size()==nodes.size()*rules.rules.size());
  CHECK(take(limeburg::analyze(nodes,2,rules)).states==analyzed.states);
  auto choice=take(limeburg::select(nodes,2,rules,"value"));
  CHECK(choice.cost==9&&choice.chosen.at(1).rule==1&&choice.chosen.at(2).rule==3);
  CHECK(choice.boundaries.size()==2&&choice.boundaries.at({1,15}).rule==1&&
        choice.boundaries.at({1,7}).rule==2);
  auto region=take(limeburg::emit_scheduler(nodes,rules,choice));
  CHECK(region.instructions.size()==1&&region.instructions[0].opcode=="MIX");
  CHECK(region.instructions[0].uses==std::vector<limeburg::NodeId>{1,1});
  CHECK(region.instructions[0].register_classes.at(1)=="G"&&region.deps.empty());

  auto incomplete=choice;
  incomplete.boundaries.erase({1,7});
  fails(limeburg::emit_scheduler(nodes,rules,incomplete),Error::Code::Conflict);
  auto reduced=rules;
  reduced.rules.erase(reduced.rules.begin()+1);
  auto missing=limeburg::select(nodes,2,reduced,"value");
  fails(missing,Error::Code::Unsatisfiable);
  CHECK(missing.error().message.find("cannot derive rhs")!=std::string::npos);

  unisel::Program graph{{{1,"input",{},{},"i64",0,false},{2,"mix",{1,1},{},"i64"}},{2}};
  graph.nodes[0].register_class="G";
  auto forest=take(limeburg::select_graph(graph,rules,"value",limeburg::GraphPolicy::PreserveShared));
  CHECK(forest.instructions.size()==1&&forest.instructions[0].uses==region.instructions[0].uses);
  CHECK(forest.instructions[0].register_classes==region.instructions[0].register_classes);
}
} // namespace

int main() {
  return test_main([]{
    ordered_covers();
    objective();
    shared_boundaries();
  });
}
