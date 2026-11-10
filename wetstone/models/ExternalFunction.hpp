#pragma once
#include "../interprocedural/EffectSummary.hpp"
namespace wetstone::models {
struct ExternalFunction {
    std::string name;
    interprocedural::EffectSummary effects;
    bool returns_nonnull=false,returns_tainted=true;
};
class Registry {
    std::map<std::string,ExternalFunction> entries_;
public:
    void add(ExternalFunction f){if(f.name.empty())throw std::invalid_argument("empty external name");entries_[f.name]=std::move(f);}
    const ExternalFunction* lookup(const std::string& name)const{
        auto it=entries_.find(name);return it==entries_.end()?nullptr:&it->second;
    }
    static Registry portable_defaults(){Registry r;
        r.add({"malloc",{false,true,true,false,false},false,false});
        r.add({"free",{false,true,false,true,false},false,false});
        r.add({"memcpy",{true,true,false,false,false},true,false});
        r.add({"read",{true,true,false,false,false},false,true});
        return r;
    }
};
}
