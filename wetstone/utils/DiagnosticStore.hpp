#pragma once
#include "Diagnostic.hpp"
#include <set>
#include <tuple>
namespace wetstone {
class DiagnosticStore {
    Diagnostics diagnostics_;
    std::set<std::tuple<std::string,std::string,std::uint32_t,std::string>> seen_;
public:
    bool add(Diagnostic d){auto key=std::make_tuple(d.rule,d.file,d.line,d.message);
        if(!seen_.insert(key).second)return false;diagnostics_.push_back(std::move(d));return true;}
    void append(const Diagnostics& items){for(const auto& d:items)add(d);}
    const Diagnostics& items()const{return diagnostics_;}
};
}
