#include "internal.hpp"

namespace limestone::vmweave {
std::string lower_abstract_c(const Specification& s, const Word& w) {
  using enum AbstractOpcode;
  auto [inputs,outputs]=effect_counts(w.effect);
  std::ostringstream out;
  // Only the instruction's inferred input window is read, and only its final
  // output window is committed. Intermediate values are C temporaries.
  out<<"  if (!vm || vm->vw_error) return;\n";
  if(!w.operands.empty()) out<<"  if (!vm->vw_current || vm->vw_current->operand_count != "
                            <<w.operands.size()<<") { vm_fail(-4); return; }\n";
  if(inputs) out<<"  if (vm->vw_sp < "<<inputs<<") { vm_fail(-2); return; }\n";
  out<<"  if (vm->vw_sp > sizeof(vm->vw_stack)/sizeof(vm->vw_stack[0])) { vm_fail(-3); return; }\n"
     <<"  size_t vw_base = vm->vw_sp - "<<inputs<<";\n";
  if(outputs>inputs)
    out<<"  if (sizeof(vm->vw_stack)/sizeof(vm->vw_stack[0]) - vw_base < "<<outputs
       <<") { vm_fail(-3); return; }\n";

  unsigned next_value=0;
  std::vector<std::string> values;
  auto push=[&](std::string_view expression) {
    auto name="vw_v"+std::to_string(next_value++);
    out<<"  cell "<<name<<" = "<<expression<<"; (void)"<<name<<";\n";
    values.push_back(std::move(name));
  };
  auto pop=[&]() {
    auto value=std::move(values.back()); values.pop_back(); return value;
  };
  for(size_t i=0;i<inputs;++i) push("vm->vw_stack[vw_base + "+std::to_string(i)+"]");
  const bool is_signed=s.cell!="uint64_t";
  const std::string lo=s.cell=="intptr_t"?"INTPTR_MIN":"INT64_MIN", hi=s.cell=="intptr_t"?"INTPTR_MAX":"INT64_MAX";
  bool halt=false;
  for(const auto& op:*w.abstract) {
    switch(op.opcode) {
      case Constant: {
        auto value=std::get<AbstractConstant>(op.argument);
        std::string literal;
        if(!is_signed) literal="UINT64_C("+std::to_string(value.magnitude)+")";
        else if(value.magnitude==(uint64_t{1}<<63)) literal="INT64_MIN";
        else literal=(value.negative?"-INT64_C(":"INT64_C(")+std::to_string(value.magnitude)+")";
        push("(cell)("+literal+")");
        break;
      }
      case Operand: push("operand"+std::to_string(std::get<uint32_t>(op.argument))); break;
      case Load: push("vm->"+std::get<std::string>(op.argument)); break;
      case Store: out<<"  vm->"<<std::get<std::string>(op.argument)<<" = "<<pop()<<";\n"; break;
      case Drop: pop(); break;
      case Dup: {auto value=values.back(); values.push_back(std::move(value)); break;}
      case Swap: std::iter_swap(values.end()-2, values.end()-1); break;
      case Eq: case Lt: case Add: case Sub: case Mul: case Div: {
        auto b=pop(), a=pop();
        auto trap=[&](const std::string& check) {out<<"  if ("<<check<<") { vm_fail(-10); return; }\n";};
        switch(op.opcode) {
          case Eq: push(a+" == "+b); break;
          case Lt: push(a+" < "+b); break;
          case Add:
            if(is_signed) trap("("+b+" > 0 && "+a+" > "+hi+" - "+b+") || ("+
                               b+" < 0 && "+a+" < "+lo+" - "+b+")");
            push(a+" + "+b); break;
          case Sub:
            if(is_signed) trap("("+b+" > 0 && "+a+" < "+lo+" + "+b+") || ("+
                               b+" < 0 && "+a+" > "+hi+" + "+b+")");
            push(a+" - "+b); break;
          case Mul:
            if(is_signed) trap("("+a+" > 0 && ("+b+" > "+hi+" / "+a+" || "+b+" < "+lo+" / "+a+")) || ("+
                               a+" < 0 && (("+b+" > 0 && "+a+" < "+lo+" / "+b+") || ("+
                               b+" < 0 && "+a+" < "+hi+" / "+b+")))");
            push(a+" * "+b); break;
          case Div:
            trap(b+" == 0"+(is_signed?" || ("+a+" == "+lo+" && "+b+" == -1)":""));
            push(a+" / "+b); break;
          default: break;
        }
        break;
      }
      case Jump: case JumpZero: {
        auto operand="operand"+std::to_string(std::get<uint32_t>(op.argument));
        if(op.opcode==JumpZero) out<<"  if ("<<pop()<<" == 0) ";
        else out<<"  ";
        out<<"vm_jump("<<operand<<");\n  if (vm->vw_error) return;\n";
        break;
      }
      case Halt: halt=true; break;
    }
  }
  for(size_t i=0;i<values.size();++i) out<<"  vm->vw_stack[vw_base + "<<i<<"] = "<<values[i]<<";\n";
  out<<"  vm->vw_sp = vw_base + "<<values.size()<<";\n";
  if(halt) out<<"  vm_halt();\n";
  return out.str();
}
}
