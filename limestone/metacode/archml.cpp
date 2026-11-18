#include "archml.hpp"
#include <ArchML.hpp>
#include <charconv>
#include <cmath>
#include <set>

namespace limestone::metacode {
namespace {
using sandstone::archml::Document;
using sandstone::archml::Node;

SourceLocation at(const std::string& source,const Node& node) {
  const auto& loc=node.location;
  return {source,loc.offset,static_cast<uint32_t>(loc.line),static_cast<uint32_t>(loc.column)};
}
[[noreturn]] void bad(const std::string& source,const Node& node,std::string message,
                      Error::Code code=Error::Code::InvalidArgument) {
  const auto loc=at(source,node);
  throw Error{code,source+":"+std::to_string(loc.line)+":"+std::to_string(loc.column)+": "+std::move(message)};
}
const Node& property(const std::string& source,const Node& node,std::string_view key) {
  if(const auto* found=node.property(key))return *found;
  bad(source,node,"missing ArchML property '"+std::string(key)+"'");
}
const Node& scalar(const std::string& source,const Node& node,std::string_view key) {
  const auto& field=property(source,node,key);
  if(field.children.size()!=2)bad(source,field,"expected one value for '"+std::string(key)+"'");
  return field.children[1];
}
const std::string& symbol(const std::string& source,const Node& node) {
  if(node.kind!=Node::Kind::Symbol)bad(source,node,"expected ArchML symbol");
  return node.text;
}
uint32_t number(const std::string& source,const Node& node,bool positive=false) {
  std::string_view text=symbol(source,node);
  int base=10;
  if(text.starts_with("0x")||text.starts_with("0X")){base=16;text.remove_prefix(2);}
  uint32_t value=0;
  const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value,base);
  if(error!=std::errc{}||end!=text.data()+text.size()||(positive&&!value))
    bad(source,node,positive?"expected positive 32-bit integer":"expected unsigned 32-bit integer");
  return value;
}
const std::string& reference(const std::string& source,const Node& node,std::string_view kind) {
  if(node.head()!="ref"||node.children.size()!=3||symbol(source,node.children[1])!=kind)
    bad(source,node,"expected reference to ArchML "+std::string(kind));
  return symbol(source,node.children[2]);
}
template<class T,class F> Result<T> result(F&& callback) {
  try {return Result<T>::ok(callback());}
  catch(const Error& error){return Result<T>::err(error);}
  catch(const sandstone::archml::Error& error){return Result<T>::err({Error::Code::Parse,error.what()});}
  catch(const std::invalid_argument& error){return Result<T>::err({Error::Code::InvalidArgument,error.what()});}
  catch(const std::bad_alloc&){return Result<T>::err({Error::Code::ResourceLimit,"ArchML allocation failed"});}
  catch(const std::exception& error){return Result<T>::err({Error::Code::Internal,error.what()});}
}
}

struct ArchMLProfile::Data {
  std::string source;
  Document document;
};

Result<ArchMLProfile> ArchMLProfile::parse(std::string_view text,std::string source) {
  return result<ArchMLProfile>([&] {
    return ArchMLProfile(std::make_shared<const Data>(Data{source,Document::parse(text,source)}));
  });
}
Result<ArchMLProfile> ArchMLProfile::load(const std::string& path) {
  return result<ArchMLProfile>([&] {
    return ArchMLProfile(std::make_shared<const Data>(Data{path,Document::load(path)}));
  });
}
const std::string& ArchMLProfile::name() const {return data_->document.name();}
uint32_t ArchMLProfile::bits() const {
  return number(data_->source,scalar(data_->source,*data_->document.find("target",name()),"bits"));
}
std::string_view ArchMLProfile::endianness() const {
  return scalar(data_->source,*data_->document.find("target",name()),"endian").text;
}
Result<std::vector<uint8_t>> ArchMLProfile::encode(std::string_view instruction,std::span<const uint64_t> operands) const {
  return result<std::vector<uint8_t>>([&] {
    const auto* form=data_->document.instruction(instruction);
    const auto context=name()+": "+std::string(instruction);
    if(!form)throw Error{Error::Code::NotFound,context+": unknown ArchML instruction"};
    if(form->base.empty())throw Error{Error::Code::Unsupported,context+": no binary encoding"};
    return sandstone::archml::runtime::encode(*form,operands);
  });
}
Result<ArchMLDecoded> ArchMLProfile::decode_one(std::span<const uint8_t> bytes) const {
  return result<ArchMLDecoded>([&] {
    sandstone::archml::runtime::Decoded decoded;
    if(!data_->document.decode(bytes,decoded))
      throw Error{Error::Code::Unsupported,name()+": no matching complete ArchML instruction"};
    const auto& form=data_->document.instructions()[decoded.instruction];
    ArchMLDecoded out{form.name,decoded.size,{}};
    for(size_t i=0;i<form.operands.size();++i)
      out.operands.push_back({form.operands[i].name,decoded.operands[i],form.operands[i].is_signed});
    return out;
  });
}
Result<ArchMLSchedule> ArchMLProfile::scheduling(std::string_view microarchitecture) const {
  return result<ArchMLSchedule>([&] {
    const auto& doc=data_->document;
    const auto& source=data_->source;
    const auto* model=doc.find("microarch",microarchitecture);
    if(!model)throw Error{Error::Code::NotFound,source+": unknown ArchML microarchitecture '"+std::string(microarchitecture)+"'"};
    ArchMLSchedule out;
    out.issue_width=number(source,scalar(source,*model,"issue-width"),true);
    std::map<std::string,ArchMLTiming> timings;
    for(const auto& ref:std::span(property(source,*model,"schedules").children).subspan(1)) {
      const auto& name=reference(source,ref,"schedule");
      const auto& schedule=*doc.find("schedule",name);
      ArchMLTiming timing;
      timing.source=at(source,schedule);
      timing.latency=number(source,scalar(source,schedule,"latency"));
      const auto& throughput=scalar(source,schedule,"throughput-cycles");
      const auto& value=symbol(source,throughput);
      const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),timing.throughput_cycles);
      if(error!=std::errc{}||end!=value.data()+value.size()||!std::isfinite(timing.throughput_cycles)||timing.throughput_cycles<=0)
        bad(source,throughput,"expected positive throughput in cycles");
      const auto& barrier=scalar(source,schedule,"barrier");
      const auto& flag=symbol(source,barrier);
      if(flag!="true"&&flag!="false")bad(source,barrier,"expected Boolean scheduling barrier");
      timing.barrier=flag=="true";
      std::set<std::string> used;
      for(const auto& use:std::span(property(source,schedule,"uses").children).subspan(1)) {
        const auto& resource=reference(source,use,"resource");
        if(!used.insert(resource).second)bad(source,use,"duplicate ArchML resource reservation");
        if(!out.resource_capacity.contains(resource)) {
          const auto& record=*doc.find("resource",resource);
          const auto& unit=scalar(source,record,"unit");
          if(symbol(source,unit)!="issue-slots")
            bad(source,unit,"unsupported ArchML resource unit (expected issue-slots)",Error::Code::Unsupported);
          out.resource_capacity.emplace(resource,number(source,scalar(source,record,"capacity"),true));
        }
        timing.resources.push_back(resource);
      }
      if(!timings.emplace(name,std::move(timing)).second)bad(source,ref,"duplicate microarchitecture schedule");
    }
    for(const auto& form:doc.instructions()) {
      const auto* instruction=doc.find(form.base.empty()?"operation":"instruction",form.name);
      if(!instruction->property("schedule"))continue;
      const auto& ref=scalar(source,*instruction,"schedule");
      if(auto found=timings.find(reference(source,ref,"schedule"));found!=timings.end())
        out.instructions.emplace(form.name,found->second);
    }
    return out;
  });
}
}
