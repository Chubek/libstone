#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
namespace moonstone::chisel {
// Stable-ID tracing heap for language-runtime integration, deliberately nonmoving.
// A host must enumerate every root; no conservative stack scanning is attempted.
class TracingHeap {
public:
 using Handle=std::uint64_t;
 struct Object {std::vector<std::byte> bytes;std::vector<Handle> references;};
 struct Stats {std::size_t retained=0,reclaimed=0;};
 Handle allocate(std::size_t bytes){if(next_==0)throw std::overflow_error("handle overflow");auto id=next_++;objects_.emplace(id,Object{std::vector<std::byte>(bytes),{}});return id;}
 Object& get(Handle h){auto it=objects_.find(h);if(it==objects_.end())throw std::out_of_range("invalid heap handle");return it->second;}
 const Object& get(Handle h)const{return const_cast<TracingHeap*>(this)->get(h);}
 void set_references(Handle h,std::vector<Handle> refs){get(h);for(auto id:refs)get(id);objects_.at(h).references=std::move(refs);}
 void pin(Handle h){get(h);roots_.insert(h);}
 void unpin(Handle h){roots_.erase(h);}
 std::size_t size()const{return objects_.size();}
 Stats collect(const std::vector<Handle>& transient_roots={}){
  std::set<Handle> marked;std::vector<Handle> todo(roots_.begin(),roots_.end());for(auto h:transient_roots){get(h);todo.push_back(h);}
  while(!todo.empty()){auto h=todo.back();todo.pop_back();if(!marked.insert(h).second)continue;for(auto child:get(h).references)todo.push_back(child);}
  Stats s;for(auto it=objects_.begin();it!=objects_.end();){if(marked.count(it->first)){++s.retained;++it;}else{++s.reclaimed;it=objects_.erase(it);}}return s;
 }
private: Handle next_=1;std::map<Handle,Object> objects_;std::set<Handle> roots_;
};
}
