#pragma once
#if __has_include("../../../core/Core.hpp")
#include "../../../core/Core.hpp"
#else
#include <moonstone/core/Core.hpp>
#endif
#include <deque>
#include <unordered_map>
namespace moonstone::chisel {
using Id=std::uint32_t;
enum class Op { Create, Use, Mutate, BorrowShared, BorrowUnique, EndBorrow, Move, Drop, Reinitialize, Escape };
struct Instruction { Op op=Op::Use; Id place=0; Id token=0; SourceSpan source; };
struct Block { Id id=0; std::vector<Instruction> instructions; std::vector<Id> successors; };
struct Function { std::vector<Block> blocks; Id entry=0; };
struct Policy { bool allow_shared_borrows=true; bool allow_unique_borrows=true;bool allow_mutation_while_shared=false; bool allow_escape_of_borrow=false; bool implicit_copy=false; std::size_t iteration_limit=100000; };
enum class Life { Uninitialized, Live, Moved, Dropped, MaybeInvalid };
struct Loan {Id place=0;bool unique=false;bool operator==(const Loan&)const=default;};
struct PlaceState { Life life=Life::Uninitialized; std::set<Id> loans; bool operator==(const PlaceState&)const=default; };
struct State {std::map<Id,PlaceState> places;std::map<Id,Loan> tokens;bool operator==(const State&)const=default;};
struct Result { Diagnostics diagnostics;bool converged=true;std::map<Id,State> block_in;std::map<Id,State> block_out; };
inline PlaceState at(const State& s,Id p){auto it=s.places.find(p);return it==s.places.end()?PlaceState{}:it->second;}
inline State join(const State& a,const State& b){State r=a;for(auto& [id,v]:r.places)if(!b.places.count(id))v.life=Life::MaybeInvalid;for(auto& [id,v]:b.places){auto it=r.places.find(id);if(it==r.places.end()){auto x=v;x.life=Life::MaybeInvalid;r.places.emplace(id,std::move(x));}else{auto& x=it->second;if(x.life!=v.life)x.life=Life::MaybeInvalid;x.loans.insert(v.loans.begin(),v.loans.end());}}for(auto& [id,l]:b.tokens){auto it=r.tokens.find(id);if(it==r.tokens.end())r.tokens[id]=l;else if(it->second!=l)throw std::invalid_argument("borrow token reused for incompatible loans");}return r;}
inline Result analyze(const Function& f,Policy policy={}){
 Result result;std::map<Id,const Block*> blocks;for(auto& b:f.blocks){if(!blocks.emplace(b.id,&b).second)throw std::invalid_argument("duplicate basic block");}
 if(!blocks.count(f.entry))throw std::invalid_argument("entry block missing");for(auto& b:f.blocks)for(auto target:b.successors)if(!blocks.count(target))throw std::invalid_argument("invalid successor");
 std::deque<Id> queue{f.entry};std::set<Id> queued{f.entry};std::set<Id> reached{f.entry};std::set<std::string> emitted;
 std::size_t steps=0;while(!queue.empty()){
  if(++steps>policy.iteration_limit){result.converged=false;break;}
  auto id=queue.front();queue.pop_front();queued.erase(id);State state=result.block_in[id];
  auto error=[&](std::string code,std::string msg,const Instruction& i){auto key=std::to_string(id)+":"+std::to_string(i.source.line)+":"+code+":"+std::to_string(i.place);if(emitted.insert(key).second)result.diagnostics.push_back({std::move(code),std::move(msg),Severity::Error,i.source});};
  for(auto& i:blocks.at(id)->instructions){auto& ps=state.places[i.place];auto require_live=[&](){if(ps.life!=Life::Live){error("CHISEL-INVALID-LIFE","use of uninitialized, moved, dropped or possibly invalid place",i);return false;}return true;};
   bool shared=false,unique=false;for(auto token:ps.loans){auto it=state.tokens.find(token);if(it!=state.tokens.end()){if(it->second.unique)unique=true;else shared=true;}}
   switch(i.op){
    case Op::Create:case Op::Reinitialize:if((ps.life==Life::Live||!ps.loans.empty()))error("CHISEL-REINIT","reinitializing a live or borrowed place",i);else ps.life=Life::Live;break;
    case Op::Use:require_live();break;
    case Op::Mutate:if(require_live()&&(unique||shared&&!policy.allow_mutation_while_shared))error("CHISEL-MUTATE-LOAN","mutation conflicts with active borrow",i);break;
    case Op::BorrowShared: if(require_live()){
       if(!policy.allow_shared_borrows||unique)error("CHISEL-SHARED-CONFLICT","shared borrow conflicts with exclusive loan or policy",i);
       else if(state.tokens.count(i.token))error("CHISEL-TOKEN-REUSED","borrow token already active",i);
       else {ps.loans.insert(i.token);state.tokens[i.token]={i.place,false};}}
       break;
    case Op::BorrowUnique:if(require_live()){
       if(!policy.allow_unique_borrows||!ps.loans.empty())error("CHISEL-UNIQUE-CONFLICT","mutable borrow conflicts with active loan or policy",i);
       else if(state.tokens.count(i.token))error("CHISEL-TOKEN-REUSED","borrow token already active",i);
       else {ps.loans.insert(i.token);state.tokens[i.token]={i.place,true};}}
       break;
    case Op::EndBorrow:{auto it=state.tokens.find(i.token);if(it==state.tokens.end()||it->second.place!=i.place)error("CHISEL-END-UNKNOWN","ending an unknown or mismatched loan",i);else{ps.loans.erase(i.token);state.tokens.erase(it);}break;}
    case Op::Move:if(require_live()){if(!ps.loans.empty())error("CHISEL-MOVE-BORROWED","cannot move borrowed place",i);else if(!policy.implicit_copy)ps.life=Life::Moved;}break;
    case Op::Drop:if(require_live()){if(!ps.loans.empty())error("CHISEL-DROP-BORROWED","cannot drop borrowed place",i);else ps.life=Life::Dropped;}break;
    case Op::Escape:if(require_live()&&!policy.allow_escape_of_borrow&&!ps.loans.empty())error("CHISEL-ESCAPE","borrowed place may escape its lifetime",i);break;
   }
  }
  result.block_out[id]=state;
  for(auto succ:blocks.at(id)->successors){bool fresh=reached.insert(succ).second;State incoming=fresh?state:join(result.block_in[succ],state);if(fresh||incoming!=result.block_in[succ]){result.block_in[succ]=std::move(incoming);if(queued.insert(succ).second)queue.push_back(succ);}}
 }
 return result;
}
}
