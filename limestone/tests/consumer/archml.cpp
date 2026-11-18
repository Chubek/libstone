#include <bin2bin/archml.hpp>
#include <schedrow/text.hpp>
#include <array>

int main(int argc,char** argv) {
  if(argc!=2)return 1;
  auto profile=limestone::metacode::ArchMLProfile::load(argv[1]);
  if(!profile)return 2;
  auto assembly=limestone::bin2bin::disassemble_archml(profile.value(),std::array<uint8_t,2>{1,0});
  if(!assembly||assembly.value()!="0: c.nop\n")return 3;
  auto model=limestone::schedrow::load_schedrow(
    "region example { instruction %1 { opcode = add; } }","inline",profile.value(),"generic");
  return !model||model.value().machine.issue_width!=1?4:0;
}
