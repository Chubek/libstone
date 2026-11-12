#pragma once
#if __has_include("../../../core/SExpr.hpp")
#include "../../../core/SExpr.hpp"
#else
#include <moonstone/core/SExpr.hpp>
#endif
#include <iostream>
namespace moonstone::syren {
struct Rule {std::string id,name,category,phase,message;Severity severity=Severity::Error;sexpr::Node pattern;std::vector<std::string> repairs;};
inline Rule parse_rule(const std::string& source){
 auto n=sexpr::Parser(source).one();if(!n.list||n.children.empty()||n.children[0].atom!="error")throw std::invalid_argument("expected (error ...) form");
 Rule r;r.id=sexpr::property_atom(n,"id");r.name=sexpr::property_atom(n,"name");r.category=sexpr::property_atom(n,"category");r.phase=sexpr::property_atom(n,"phase");r.message=sexpr::property_atom(n,"message");auto severity=sexpr::property_atom(n,"severity","error");
 if(severity=="note")r.severity=Severity::Note;else if(severity=="warning")r.severity=Severity::Warning;else if(severity=="fatal")r.severity=Severity::Fatal;else if(severity!="error")throw std::invalid_argument("invalid severity");
 auto p=sexpr::property(n,"pattern");if(!p||p->children.size()!=2||r.id.empty()||r.message.empty())throw std::invalid_argument("incomplete diagnostic rule");r.pattern=p->children[1];
 if(auto repairs=sexpr::property(n,"repairs"))for(std::size_t i=1;i<repairs->children.size();++i)if(auto& x=repairs->children[i];x.list&&x.children.size()>1&&!x.children[1].list)r.repairs.push_back(x.children[1].atom);
 return r;
}
inline std::string interpolate(std::string text,const sexpr::Bindings& bindings){for(auto& [k,v]:bindings){std::string key="{"+k.substr(1)+"}";std::size_t pos=0;auto replacement=sexpr::print(v);while((pos=text.find(key,pos))!=std::string::npos){text.replace(pos,key.size(),replacement);pos+=replacement.size();}}return text;}
class Catalog {
 std::map<std::string,Rule> rules_;
public:
 void add(Rule r){if(!rules_.emplace(r.id,std::move(r)).second)throw std::invalid_argument("duplicate diagnostic id");}
 std::size_t size()const{return rules_.size();}
 Diagnostics diagnose(const sexpr::Node& tree,SourceSpan source={})const{
  Diagnostics out;for(auto& [id,r]:rules_)if(auto bindings=sexpr::match(r.pattern,tree)){Diagnostic d;d.id=id;d.message=interpolate(r.message,*bindings);d.severity=r.severity;d.primary=source;for(auto& repair:r.repairs)d.notes.push_back("repair strategy: "+repair+" (unverified)");out.push_back(std::move(d));}return out;
 }
};
class Sink {public:virtual ~Sink()=default;virtual void emit(const Diagnostic&)=0;};
class StderrSink final:public Sink{std::ostream& out_;public:explicit StderrSink(std::ostream& out=std::cerr):out_(out){}void emit(const Diagnostic& d)override{out_<<format(d)<<'\n';}};
inline std::string to_sarif(const Diagnostics& ds,std::string_view tool="Moonstone Syren"){
 std::string o="{\"version\":\"2.1.0\",\"$schema\":\"https://json.schemastore.org/sarif-2.1.0.json\",\"runs\":[{\"tool\":{\"driver\":{\"name\":\""+json_escape(tool)+"\"}},\"results\":[";
 bool first=true;for(auto& d:ds){if(!first)o+=',';first=false;o+="{\"ruleId\":\""+json_escape(d.id)+"\",\"level\":\""+(d.severity==Severity::Fatal?"error":name(d.severity))+"\",\"message\":{\"text\":\""+json_escape(d.message)+"\"}";
 if(!d.primary.file.empty())o+=",\"locations\":[{\"physicalLocation\":{\"artifactLocation\":{\"uri\":\""+json_escape(d.primary.file)+"\"},\"region\":{\"startLine\":"+std::to_string(std::max(1u,d.primary.line))+",\"startColumn\":"+std::to_string(std::max(1u,d.primary.column))+"}}}]";
 o+='}';}return o+"]}]}";
}
class SarifSink final:public Sink{Diagnostics collected_;public:void emit(const Diagnostic& d)override{collected_.push_back(d);}std::string serialize()const{return to_sarif(collected_);}const Diagnostics& diagnostics()const{return collected_;}};
}
