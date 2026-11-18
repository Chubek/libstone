#pragma once
#include "runtime.h"
#include <string_view>

namespace limestone::traceml::detail {
struct Primitive { std::string_view name; traceml_opcode_v1 opcode; size_t arity; };
inline constexpr Primitive primitives[] = {
  {"add",TRACEML_ADD,2}, {"sub",TRACEML_SUB,2}, {"mul",TRACEML_MUL,2},
  {"eq",TRACEML_EQ,2}, {"lt",TRACEML_LT,2}, {"neg",TRACEML_NEG,1}
};
inline const Primitive* primitive(std::string_view name) {
  for(const auto& p:primitives)if(p.name==name)return &p;
  return nullptr;
}
}
