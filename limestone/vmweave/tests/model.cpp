#include "vmweave/vmweave.hpp"
#include "metacode/machine-ir/bridge.hpp"
#include "tests/test.hpp"
using namespace limestone;
using namespace limestone::vmweave;
int main() { return test_main([] {
  auto spec=take(load_lua(R"(local v=require('vmweave'); return v.vm {
    name='Example', components={'insncode'}, execution='none',
    fields={{name='z',type='u8'},{name='a',type='i64'}},
    instructions={{name='Z',opcode=3,semantics='vm->a++;'},{name='A',semantics='/* }c ; */\nvm->z++;'}}
  })","example.lua"));
  CHECK(spec.fields[0].name=="a");
  auto m=take(lower(spec)); CHECK(m.words[0].name=="A" && m.words[0].opcode==0);
  auto stk=take(print_stk(m)); auto parsed=take(parse_stk(stk));
  CHECK(take(print_stk(parsed))==stk);
  CHECK(take(emit_c(parsed))==take(emit_c(m)));
  auto artifacts=take(generate(m)); CHECK(artifacts.files.size()==4);
  CHECK(artifacts.files.contains("Example.insncode")); CHECK(!artifacts.files.contains("Example.object"));
  auto duplicate=spec; duplicate.instructions[0].opcode=3;
  fails(validate(duplicate),Error::Code::Conflict);
  auto bad=spec; bad.instructions[0].stack_effect="( missing )"; CHECK(!lower(bad));
  bad=spec; bad.components.push_back("dispatc"); CHECK(!lower(bad));
  bad=spec; bad.fields.push_back({"vw_sp","size",0}); CHECK(!lower(bad));
  bad=spec; bad.instructions[0].semantics=std::string("a\0b",3); CHECK(!lower(bad));
  CHECK(!parse_stk(stk+"unexpected")); CHECK(!parse_stk("stk-00 999"));
  CHECK(!parse_stk(stk.substr(0,stk.size()/2)));
  CHECK(!load_lua("return {name='Bad',instructions={{name='X',opcode=1.5}}}","bad.lua"));
  CHECK(!load_lua("return {name='Bad',instructions={{name='X',opcode=0/0}}}","bad.lua"));
  CHECK(!load_lua("return {name='Bad',hooks='yes'}","bad.lua"));
  CHECK(!load_lua("return {name='Bad',instructions={{name='X',semantics=7}}}","bad.lua"));
  CHECK(!load_lua("return {name='Bad',components={'insncode'},subsystems={insncode=7}}","bad.lua"));
  CHECK(!load_lua("return {name='Bad',instructions={{name='vm'}}}","bad.lua"));
  CHECK(!load_lua("return {name='Bad',surprise=1}","bad.lua"));
  CHECK(!load_lua("return {name='Bad',components={'object'}}","bad.lua"));
  CHECK(!load_lua("return {name='Bad',instructions={{name='X',operands={'ptr'}}}}","bad.lua"));
  CHECK(!load_lua("error('specific failure')","bad.lua"));
  auto diagnostic=load_lua("local v=require('vmweave'); return v.vm {name='Bad',instructions={{name='X',opcode=2},{name='Y',opcode=2}}}","duplicate.lua");
  CHECK(!diagnostic && diagnostic.error().message.find("duplicate.lua")!=std::string::npos);
  auto c=take(load_lua("local v=require('vmweave'); local m=v.vm('Counter'); m:instruction('inc',{body='(void)vm;'}); assert(m:emit():find('Counter_inc')); return m"));
  CHECK(c.name=="Counter");

  auto mixed=take(load_lua(R"(
    local v=require("vmweave")
    local m=v.vm("Mixed")
    m:instruction("INC",{abstract="const 1; # step\nadd;"})
    m:instruction("HALT",{abstract="halt;"})
    m:instruction("NOP",{abstract=""})
    m:instruction("OTHER",{body="/* raw C }c ; */"})
    assert(m:emit():find("INT64_C"))
    return m
  )","mixed.lua"));
  CHECK(stk.starts_with("stk-00 1\n"));
  auto mixed_module=take(lower(mixed));
  CHECK(mixed_module.words[0].name=="HALT" && mixed_module.words[0].flow=="halt");
  const auto& inc=mixed_module.words[1];
  CHECK(inc.name=="INC" && inc.effect=="( in0 -- out0 )" && inc.c_body.empty());
  CHECK(inc.abstract && inc.abstract->size()==2);
  CHECK(std::get<AbstractConstant>(inc.abstract->front().argument).magnitude==1);
  CHECK(inc.abstract->back().opcode==AbstractOpcode::Add && inc.abstract->back().offset==16);
  CHECK(mixed_module.words[2].abstract && mixed_module.words[2].abstract->empty());
  auto mixed_stk=take(print_stk(mixed_module));
  CHECK(mixed_stk.starts_with("stk-00 2\n"));
  CHECK(mixed_stk.find("/* raw C }c ; */")!=std::string::npos);
  auto mixed_roundtrip=take(parse_stk(mixed_stk));
  CHECK(take(print_stk(mixed_roundtrip))==mixed_stk);
  CHECK(take(emit_c(mixed_roundtrip))==take(emit_c(mixed_module)));
  auto replaced=mixed_stk; replaced[7]='1';
  CHECK(!parse_stk(replaced));
  replaced=mixed_stk; replaced.replace(replaced.find("add;"),4,"xxx;");
  CHECK(!parse_stk(replaced));
  replaced=mixed_stk; replaced.replace(replaced.find(" in0"),4,"");
  CHECK(!parse_stk(replaced));
  replaced=mixed_stk; replaced.replace(replaced.find("}a"),2,"}c");
  CHECK(!parse_stk(replaced));

  const auto check_abstract_error=[](std::string_view body,std::string_view metadata,std::string_view expected) {
    auto text=std::string("return {name='Bad', instructions={{name='OP',source={file='operation.vw', line=9},abstract=[=[")
      +std::string(body)+"]=],"+std::string(metadata)+"}}}";
    auto rejected=load_lua(text,"bad.lua");
    CHECK(!rejected && rejected.error().message.find(expected)!=std::string::npos);
  };
  check_abstract_error("const 1;\nmissing;","","operation.vw:10:1");
  check_abstract_error("const 1","","semicolon");
  check_abstract_error("drop 0;","","takes no argument");
  check_abstract_error("const abc;","","decimal cell value");
  check_abstract_error("const 18446744073709551616;","","constant is out of range");
  check_abstract_error("const 2147483648;","","out of range for intptr_t");
  check_abstract_error("jump 16;","operands={'label'}","operand index in 0..15");
  check_abstract_error("operand 1;","operands={'cell'}","undeclared operand index");
  check_abstract_error("jump 0;","operands={'cell'}","requires a label operand");
  check_abstract_error("operand 0;","operands={'label'}","requires a cell operand");
  check_abstract_error("load missing;","","unknown state field");
  check_abstract_error("halt; const 0;","","control transfer must end the body");
  check_abstract_error("halt;","flow='next'","declared flow disagrees");
  check_abstract_error("add;","stack_effect='( x -- y )'","declared stack effect disagrees");
  check_abstract_error("halt;","semantics=''","mutually exclusive");
  CHECK(!load_lua("return {name='Tiny',stack={capacity=1},instructions={{name='DUP',abstract='dup;'}}}"));
  CHECK(!load_lua("return {name='Array',stack={cell='int64_t'},fields={{name='x',type='i64',count=2}},instructions={{name='LOAD',abstract='load x;'}}}"));
  CHECK(!load_lua("return {name='Narrow',stack={cell='int64_t'},fields={{name='x',type='i8'}},instructions={{name='LOAD',abstract='load x;'}}}"));
  CHECK(!load_lua("return {name='Unsigned',stack={cell='uint64_t'},instructions={{name='NEG',abstract='const -1;'}}}"));
  auto op=take(parse_abstract("  const -0;"));
  CHECK(op[0].offset==2 && std::get<AbstractConstant>(op[0].argument).magnitude==0 && !std::get<AbstractConstant>(op[0].argument).negative);
  fails(parse_abstract(std::string(65537,' ')),Error::Code::ResourceLimit);
  auto ops=std::string{};
  for(int i=0;i<256;++i) ops+="drop;";
  CHECK(take(parse_abstract(ops)).size()==256);
  fails(parse_abstract(ops+"drop;"),Error::Code::ResourceLimit);
  auto invalid_ast=mixed_module;
  invalid_ast.words[0].abstract->front().argument=uint32_t{0};
  CHECK(!emit_c(invalid_ast));
  invalid_ast=mixed_module;
  invalid_ast.words[0].abstract->front().opcode=static_cast<AbstractOpcode>(-1);
  CHECK(!print_stk(invalid_ast));
  invalid_ast=mixed_module;
  invalid_ast.words[0].c_body="vm_halt();";
  CHECK(!generate(invalid_ast));
  Specification oversized; oversized.name="Oversized"; oversized.cell="int64_t";
  std::string field(32768,'a');
  oversized.fields.push_back({field,"i64"});
  Instruction load; load.name="LOAD"; load.abstract=AbstractProgram{
    {AbstractOpcode::Load,field}, {AbstractOpcode::Load,field}
  };
  oversized.instructions.push_back(std::move(load));
  CHECK(!lower(std::move(oversized)));

  struct InvalidAdapter : MachineIRAdapter {
    Result<std::string> lower(const Module&) override {return Result<std::string>::ok("{}");}
  } invalid;
  CHECK(!emit_machineir(m,invalid));
  struct EmptyAdapter : MachineIRAdapter {
    Result<std::string> lower(const Module&) override {
      machineir_bridge::RegionExchange x; x.target="generic";
      return machineir_bridge::serialize(x);
    }
  } empty;
  Specification empty_spec; empty_spec.name="Empty"; empty_spec.execution="none";
  CHECK(machineir_bridge::deserialize(take(emit_machineir(take(lower(empty_spec)),empty))));
}); }
