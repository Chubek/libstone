#include "bin2bin/archml.hpp"
#include "schedrow/archml.hpp"
#include "schedrow/text.hpp"
#include "test.hpp"
#include <array>
#include <bit>
#include <filesystem>

using namespace limestone;

int main(int argc,char** argv) {
  CHECK(argc==2);
  const std::filesystem::path specs=argv[1];
  auto source=metacode::ArchMLProfile::load((specs/"riscv64.aml").string());
  CHECK(source);
  auto profile=source.value();
  CHECK(profile.name()=="riscv64"&&profile.bits()==64&&profile.endianness()=="little");
  const std::array<uint64_t,3> registers{1,2,3};
  auto encoded=profile.encode("add",registers);
  CHECK(encoded&&encoded.value()==std::vector<uint8_t>({0xb3,0x00,0x31,0x00}));
  auto branch=profile.encode("beq",std::array<uint64_t,3>{1,2,std::bit_cast<uint64_t>(int64_t{-4})});
  CHECK(branch&&branch.value()==std::vector<uint8_t>({0xe3,0x8e,0x20,0xfe}));
  auto invalid=profile.encode("beq",std::array<uint64_t,3>{1,2,3});
  CHECK(!invalid&&invalid.error().code==Error::Code::InvalidArgument);
  auto compact=profile.encode("c.nop",{});
  CHECK(compact&&compact.value()==std::vector<uint8_t>({1,0}));
  auto unknown=profile.encode("not_real",{});
  CHECK(!unknown&&unknown.error().code==Error::Code::NotFound);
  auto bytes=encoded.value();
  bytes.insert(bytes.end(),compact.value().begin(),compact.value().end());
  bytes.insert(bytes.end(),branch.value().begin(),branch.value().end());
  auto assembly=bin2bin::disassemble_archml(profile,bytes,0x40);
  CHECK(assembly&&assembly.value()=="40: add rd=1 rs1=2 rs2=3\n44: c.nop\n46: beq rs1=1 rs2=2 disp=-4\n");
  bytes.pop_back();
  auto truncated=bin2bin::disassemble_archml(profile,bytes,0x40);
  CHECK(!truncated&&truncated.error().message.find("0x46")!=std::string::npos);
  CHECK(!bin2bin::disassemble_archml(profile,bytes,UINT64_MAX));

  auto timing=profile.scheduling("generic");
  CHECK(timing&&timing.value().issue_width==1);
  CHECK(timing.value().instructions.at("mul").latency==4);
  CHECK(timing.value().instructions.at("beq").barrier);
  CHECK(timing.value().instructions.at("mul").source.line>1);
  schedrow::Region region;
  schedrow::Instruction mul{1,"mul"},add{2,"add"};
  mul.defs={7};
  add.uses={7};
  region.instructions={mul,add};
  auto input=schedrow::bind_archml_schedule(region,profile,"generic");
  CHECK(input&&input.value().region.instructions[0].latency==4);
  CHECK(region.instructions[0].latency==1);
  const auto& bound=input.value();
  auto order=schedrow::schedule(bound.region,bound.machine);
  CHECK(order&&order.value().size()==2);
  CHECK(order.value()[0].id==1&&order.value()[0].cycle==0);
  CHECK(order.value()[1].id==2&&order.value()[1].cycle==4);
  CHECK(schedrow::verify(bound.region,bound.machine,order.value()));
  constexpr auto text=R"(
    machine_model example { critical_path = true; }
    region chain {
      instruction %1 { opcode = mul; def %7; }
      instruction %2 { opcode = add; use %7; }
    })";
  CHECK(!schedrow::load_schedrow(text));
  auto document=schedrow::load_schedrow(text,"chain.schedrow",profile,"generic");
  CHECK(document&&document.value().machine.critical_path);
  auto text_order=schedrow::schedule(document.value().regions.front().region,document.value().machine);
  CHECK(text_order&&text_order.value()[1].cycle==4);
  CHECK(!schedrow::bind_archml_schedule(region,profile,"invented"));
  region.instructions[0].opcode="invented";
  CHECK(!schedrow::bind_archml_schedule(region,profile,"generic"));
  auto malformed=metacode::ArchMLProfile::parse("(archml 1 bad", "bad.aml");
  CHECK(!malformed&&malformed.error().code==Error::Code::Parse);
  auto invalid_model=metacode::ArchMLProfile::parse(R"(
(archml 1 test
 (target test (bits 0x40) (endian big))
 (microarch broken (issue-width 0) (schedules)))
)","invalid.aml");
  CHECK(invalid_model&&invalid_model.value().bits()==64&&invalid_model.value().endianness()=="big");
  auto rejected=invalid_model.value().scheduling("broken");
  CHECK(!rejected&&rejected.error().message.find("invalid.aml:4:")!=std::string::npos);
}
