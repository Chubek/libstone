#pragma once
#if __has_include("../../../core/Core.hpp")
#include "../../../core/Core.hpp"
#else
#include <moonstone/core/Core.hpp>
#endif
#include <variant>
namespace moonstone::exoml {
// Safe-by-default interpreter for the declarative core of ExoML.
// Native host capabilities are registered explicitly; no implicit dynamic loading.
using Value=std::variant<std::monostate,std::int64_t,bool,std::string,std::vector<std::string>>;
inline std::string stringify(const Value& v){if(auto p=std::get_if<std::string>(&v))return *p;if(auto p=std::get_if<std::int64_t>(&v))return std::to_string(*p);if(auto p=std::get_if<bool>(&v))return *p?"true":"false";if(auto p=std::get_if<std::vector<std::string>>(&v)){std::string s="[";for(auto& x:*p){if(s.size()>1)s+=',';s+=x;}return s+"]";}return "null";}
struct Command {std::string binding, operation, function;std::vector<std::string> arguments;SourceSpan source;};
struct Module {std::vector<Command> commands;};
class Parser {
 std::string_view input_;std::size_t pos_=0;std::uint32_t line_=1,col_=1;
 void advance(){if(input_[pos_++]=='\n'){++line_;col_=1;}else ++col_;}
 void space(){while(pos_<input_.size()){if(std::isspace((unsigned char)input_[pos_])){advance();continue;}if(input_[pos_]=='-'&&pos_+1<input_.size()&&input_[pos_+1]=='-'){while(pos_<input_.size()&&input_[pos_]!='\n')advance();continue;}break;}}
 bool consume(char c){space();if(pos_<input_.size()&&input_[pos_]==c){advance();return true;}return false;}
 std::string token(){space();if(pos_==input_.size())throw std::invalid_argument("unexpected EOF");if(input_[pos_]=='"'){advance();std::string s;bool closed=false;while(pos_<input_.size()){char c=input_[pos_];advance();if(c=='"'){closed=true;break;}if(c=='\\'){if(pos_==input_.size())throw std::invalid_argument("invalid string escape");c=input_[pos_];advance();if(c=='n')c='\n';else if(c=='t')c='\t';else if(c!='"'&&c!='\\')throw std::invalid_argument("invalid string escape");}s+=c;}if(!closed)throw std::invalid_argument("unterminated literal");return '"'+s+'"';}
 auto start=pos_;while(pos_<input_.size()){char c=input_[pos_];if(std::isspace((unsigned char)c)||c=='('||c==')'||c==','||c==';')break;advance();}if(start==pos_)throw std::invalid_argument("unexpected punctuation");return std::string(input_.substr(start,pos_-start));}
public:explicit Parser(std::string_view s):input_(s){}
 Module parse(){Module result;space();while(pos_<input_.size()){Command c;c.source={"",line_,col_,line_,col_};c.binding=token();c.operation=token();if(c.operation!="~setconst!"&&c.operation!="~setvar!"&&c.operation!="~get!"&&c.operation!="~hatch!")throw std::invalid_argument("unsupported ExoML operator: "+c.operation);
 auto call=token();auto paren=call.find('(');if(paren!=std::string::npos){c.function=call.substr(0,paren);if(call.substr(paren)!="(")throw std::invalid_argument("invalid call token");}else c.function=call;
 if(paren==std::string::npos){if(!consume('('))throw std::invalid_argument("expected '('");}
 if(!consume(')')){for(;;){c.arguments.push_back(token());if(consume(')'))break;if(!consume(','))throw std::invalid_argument("expected ',' or ')'");}}
 if(!consume(';'))throw std::invalid_argument("expected ';'");c.source.end_line=line_;c.source.end_column=col_;result.commands.push_back(std::move(c));space();}return result;}
};
class Interpreter {
public:
 using HostFunction=std::function<Value(const std::vector<Value>&)>;
 struct Registered {HostFunction callback;bool privileged=false;};
private:
 std::map<std::string,Value> symbols_;std::set<std::string> constants_;std::map<std::string,Registered> services_;std::set<std::string> permitted_;bool allow_hatch_=false;
 static Value literal(const std::string& arg,const std::map<std::string,Value>& symbols){if(arg.size()>=2&&arg.front()=='"'&&arg.back()=='"')return arg.substr(1,arg.size()-2);if(arg=="true")return true;if(arg=="false")return false;if(arg.size()>1&&arg.front()=='$'){auto it=symbols.find(arg.substr(1));if(it==symbols.end())throw std::invalid_argument("unbound ExoML value: "+arg);return it->second;}std::size_t n=0;try{auto x=std::stoll(arg,&n);if(n==arg.size())return static_cast<std::int64_t>(x);}catch(const std::exception&){}throw std::invalid_argument("unsupported literal/reference: "+arg);}
public:
 void register_service(std::string name,HostFunction fn,bool privileged=false){if(name.empty()||!fn)throw std::invalid_argument("invalid service");if(!services_.emplace(std::move(name),Registered{std::move(fn),privileged}).second)throw std::invalid_argument("service already registered");}
 void allow_service(std::string name){permitted_.insert(std::move(name));}
 void enable_hatch(bool enable){allow_hatch_=enable;}
 const std::map<std::string,Value>& symbols()const{return symbols_;}
 Diagnostics execute(const Module& module){Diagnostics errors;for(auto& c:module.commands){try{
    auto it=services_.find(c.function);if(it==services_.end())throw std::invalid_argument("unknown service: "+c.function);
    if(it->second.privileged&&(!allow_hatch_||!permitted_.count(c.function)||c.operation!="~hatch!"))throw std::runtime_error("service requires explicit hatch capability");
    if(c.operation=="~hatch!"&&!allow_hatch_)throw std::runtime_error("hatching disabled");
    if(constants_.count(c.binding))throw std::runtime_error("cannot reassign constant: "+c.binding);
    std::vector<Value> args;for(auto& arg:c.arguments)args.push_back(literal(arg,symbols_));
    auto v=it->second.callback(args);symbols_[c.binding]=std::move(v);if(c.operation=="~setconst!")constants_.insert(c.binding);
   }catch(const std::exception& e){errors.push_back({"EXOML-EVAL",e.what(),Severity::Error,c.source});}}
   return errors;}
};
}
