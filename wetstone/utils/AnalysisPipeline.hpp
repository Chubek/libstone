#pragma once
#include "AnalysisManager.hpp"
#include "Diagnostic.hpp"
namespace wetstone {
using AnalysisTask=std::function<Diagnostics(const ir::Function&,AnalysisManager&)>;
class AnalysisPipeline {
    std::vector<std::pair<std::string,AnalysisTask>> tasks_;
public:
    void register_analysis(std::string name,AnalysisTask task){
        if(name.empty()||!task)throw std::invalid_argument("invalid analysis");
        for(const auto& [existing,_]:tasks_)if(existing==name)throw std::invalid_argument("duplicate analysis");
        tasks_.emplace_back(std::move(name),std::move(task));
    }
    Diagnostics analyze(const ir::Function& f,AnalysisManager& manager)const{
        f.validate();Diagnostics out;
        for(const auto& [name,task]:tasks_){(void)name;auto diagnostics=task(f,manager);
            out.insert(out.end(),diagnostics.begin(),diagnostics.end());}
        return out;
    }
    std::size_t size()const{return tasks_.size();}
};
}
