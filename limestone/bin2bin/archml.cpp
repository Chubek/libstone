#include "archml.hpp"
#include <bit>
#include <sstream>

namespace limestone::bin2bin {
Result<std::string> disassemble_archml(const metacode::ArchMLProfile& profile,std::span<const uint8_t> bytes,uint64_t address) {
  if(bytes.size()>UINT64_MAX-address)
    return Result<std::string>::err({Error::Code::InvalidArgument,"ArchML disassembly address range overflow"});
  std::ostringstream out;
  while(!bytes.empty()) {
    auto decoded=profile.decode_one(bytes);
    if(!decoded) {
      std::ostringstream message;
      message<<"at 0x"<<std::hex<<address<<": "<<decoded.error().message;
      return Result<std::string>::err({decoded.error().code,message.str()});
    }
    const auto& form=decoded.value();
    out<<std::hex<<address<<": "<<form.instruction<<std::dec;
    for(const auto& op:form.operands) {
      out<<' '<<op.name<<'=';
      if(op.is_signed)out<<std::bit_cast<int64_t>(op.value);else out<<op.value;
    }
    out<<'\n';
    address+=form.size;
    bytes=bytes.subspan(form.size);
  }
  return Result<std::string>::ok(out.str());
}
}
