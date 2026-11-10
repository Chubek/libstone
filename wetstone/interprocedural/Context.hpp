#pragma once
#include <deque>
#include <string>
#include <vector>
namespace wetstone::interprocedural {
struct CallString {
    std::vector<std::string> sites;
    std::size_t k=2;
    CallString push(const std::string& callsite)const{
        CallString r=*this;r.sites.push_back(callsite);
        if(r.sites.size()>k)r.sites.erase(r.sites.begin(),r.sites.begin()+(r.sites.size()-k));
        return r;
    }
    friend bool operator==(const CallString&,const CallString&)=default;
};
}
