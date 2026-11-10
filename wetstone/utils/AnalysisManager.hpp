#pragma once
#include "Program.hpp"
#include <any>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <typeindex>
namespace wetstone {
// Per-function versioned cache. All mutating passes must advance the revision.
class AnalysisManager {
    struct Key {std::string function,analysis;friend auto operator<=>(const Key&,const Key&)=default;};
    struct Entry{std::uint64_t revision;std::any value;std::type_index type{typeid(void)};};
    std::map<Key,Entry> cache_;
public:
    void invalidate(const std::string& function){for(auto it=cache_.begin();it!=cache_.end();)
        if(it->first.function==function)it=cache_.erase(it);else ++it;}
    void invalidate_all(){cache_.clear();}
    template<class T,class Builder>const T& get(const std::string& analysis,const ir::Function& function,
                                                 std::uint64_t revision,Builder&& build){
        Key key{function.name,analysis};auto it=cache_.find(key);
        if(it==cache_.end()||it->second.revision!=revision){Entry value{revision,std::any(std::forward<Builder>(build)()),typeid(T)};
            it=cache_.insert_or_assign(key,std::move(value)).first;}
        if(it->second.type!=std::type_index(typeid(T)))throw std::logic_error("analysis cache type mismatch");
        return std::any_cast<const T&>(it->second.value);
    }
};
}
