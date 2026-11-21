#include <cephyr/lower.hpp>
#include <cephyr/preprocess.hpp>

#include "Backend.h"

#include <ASTNode.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cephyr::detail {
namespace {

using cparser::ASTNodeKind;

CType invalid_type() { return {}; }

CType void_type() {
  CType type;
  type.kind = CType::Kind::void_;
  return type;
}

CType size_type() { return integer_type(64, false, 4); }
CType difference_type() { return integer_type(64, true, 4); }
CType int_type() { return integer_type(32, true); }

Expr invalid_expr() { return {}; }
LValue invalid_lvalue() { return {}; }

std::string id(Id value) { return std::to_string(value); }

std::string binary_operands(Id lhs, Id rhs) {
  return id(lhs) + " " + id(rhs);
}

bool is_void(const CType &type) { return type.kind == CType::Kind::void_; }

CType unqualified(CType type) {
  type.clear_qualifiers();
  return type;
}

CType array_element_type(const CType &array) {
  if (array.kind != CType::Kind::array || !array.element)
    return {};
  auto element = *array.element;
  element.is_const = element.is_const || array.is_const;
  element.is_volatile = element.is_volatile || array.is_volatile;
  element.is_restrict = element.is_restrict || array.is_restrict;
  return element;
}

bool is_object_pointer(const CType &type) {
  return type.pointer() && type.element &&
         type.element->kind != CType::Kind::function &&
         type.element->kind != CType::Kind::void_;
}

bool is_void_pointer(const CType &type) {
  return type.pointer() && type.element &&
         type.element->kind == CType::Kind::void_;
}

bool is_unqualified_void_pointer(const CType &type) {
  return is_void_pointer(type) && !type.element->is_const &&
         !type.element->is_volatile && !type.element->is_restrict;
}

bool is_null_pointer_constant(const Expr &value) {
  return value.constant && *value.constant == 0 &&
         (value.type.integer() || is_unqualified_void_pointer(value.type));
}

bool complete_object_pointer(const CType &type) {
  return is_object_pointer(type) && size_of(*type.element).has_value();
}

bool drops_qualifiers(const CType &from, const CType &to) {
  if (!from.pointer() || !to.pointer() || !from.element || !to.element)
    return false;
  return (from.element->is_const && !to.element->is_const) ||
         (from.element->is_volatile && !to.element->is_volatile) ||
         (from.element->is_restrict && !to.element->is_restrict);
}

bool compatible_object_pointers(const CType &lhs, const CType &rhs) {
  if (!lhs.pointer() || !rhs.pointer() || lhs.function_pointer() ||
      rhs.function_pointer())
    return false;
  if (is_void_pointer(lhs) || is_void_pointer(rhs))
    return true;
  return lhs.element && rhs.element &&
         compatible(unqualified(*lhs.element), unqualified(*rhs.element));
}

CType pointer_composite(const CType &lhs, const CType &rhs) {
  const CType *selected = lhs.element.get();
  if (is_void_pointer(lhs) || is_void_pointer(rhs))
    selected = is_void_pointer(lhs) ? lhs.element.get() : rhs.element.get();
  else if (selected && selected->kind == CType::Kind::array && !selected->count &&
           rhs.element && rhs.element->kind == CType::Kind::array)
    selected = rhs.element.get(); // Keep a known bound in the composite type.
  if (!selected)
    return {};
  auto pointee = *selected;
  pointee.is_const = (lhs.element && lhs.element->is_const) ||
                     (rhs.element && rhs.element->is_const);
  pointee.is_volatile = (lhs.element && lhs.element->is_volatile) ||
                        (rhs.element && rhs.element->is_volatile);
  pointee.is_restrict = (lhs.element && lhs.element->is_restrict) ||
                        (rhs.element && rhs.element->is_restrict);
  return pointer_type(pointee);
}

// Type inference normally retains array/function designators so sizeof and &
// can inspect them. Operators that perform the C value conversions request
// decay explicitly.
CType decay(CType type) {
  if (type.kind == CType::Kind::array && type.element)
    return pointer_type(array_element_type(type));
  if (type.kind == CType::Kind::function)
    return pointer_type(type);
  return unqualified(std::move(type));
}

bool stdarg_builtin(std::string_view name) {
  return name == "__cephyr_va_start" || name == "__cephyr_va_arg" ||
         name == "__cephyr_va_end";
}

bool type_modifiable(Node *, const CType &, const TypeContext &);

bool scalar_variadic_signature(const CType &type) {
  return type.element && (is_void(*type.element) || type.element->scalar()) &&
         std::all_of(type.params.begin(), type.params.end(),
                     [](const CType &param) { return param.scalar(); });
}

// The header's type argument is encoded as a cast to a pointer to the
// requested type. It is a type carrier only and must not be evaluated.
CType stdarg_builtin_type(const TypeContext &context, Node *node, std::string_view name,
                         const std::vector<Node *> &args) {
  const auto error = [&](std::string_view message) {
    context.diagnostics.error("cephyr.stdarg", message, location(context.source, node));
    return invalid_type();
  };
  if (!context.source.native_scalar_stdarg())
    return error("stdarg builtins require a bundled SysV AMD64, AAPCS64 or RV64IM LP64 backend");
  const bool rv64 = context.source.native_rv64_soft_fp;
  const bool end = name == "__cephyr_va_end";
  if (end && !rv64)
    return error("__cephyr_va_end requires the bundled RV64IM LP64 backend");
  if (args.size() != (end ? 1u : 2u))
    return error("invalid stdarg builtin argument count");
  const bool aarch64 = context.source.native_aarch64_stdarg;
  const auto ap_type = expression_type(args[0], context);
  if (rv64) {
    if (!is_unqualified_void_pointer(ap_type) || !type_modifiable(args[0], ap_type, context))
      return error("stdarg builtin requires a writable RV64IM LP64 va_list");
    if (end)
      return void_type();
  } else {
    const auto ap = decay(ap_type);
    const auto *record = ap.pointer() && ap.element && !ap.element->is_const
                             ? ap.element->record : nullptr;
    const auto layout_error = aarch64 ? "stdarg builtin requires a writable AAPCS64 va_list"
                                      : "stdarg builtin requires a writable SysV AMD64 va_list";
    if (!record || record->is_union || record->contains_bit_field ||
        record->size != (aarch64 ? 32 : 24) ||
        record->alignment != 8 || record->members.size() != (aarch64 ? 5 : 4))
      return error(layout_error);
    for (std::size_t i = 0; i != record->members.size(); ++i) {
      constexpr std::string_view amd64_names[]{
          "__gp_offset", "__fp_offset", "__overflow_arg_area", "__reg_save_area"};
      constexpr unsigned amd64_offsets[]{0, 4, 8, 16};
      constexpr std::string_view aarch64_names[]{
          "__stack", "__gr_top", "__vr_top", "__gr_offs", "__vr_offs"};
      constexpr unsigned aarch64_offsets[]{0, 8, 16, 24, 28};
      const auto &member = record->members[i];
      if (member.name != (aarch64 ? aarch64_names[i] : amd64_names[i]) ||
          member.offset != (aarch64 ? aarch64_offsets[i] : amd64_offsets[i]) ||
          member.type.is_const ||
          ((aarch64 ? i >= 3 : i < 2) ? !compatible(member.type, integer_type(32, aarch64))
                                      : !is_void_pointer(member.type)))
        return error(layout_error);
    }
  }
  if (name == "__cephyr_va_start") {
    const auto *f = context.function;
    const auto last = args[1]->getKind() == cparser::NK_IDENT_NODE && f
                          ? f->find_local(identifier(args[1])) : std::nullopt;
    if (!last || f->va_start_parameter == no_value || last->address != f->va_start_parameter)
      return error("va_start requires the rightmost eligible named parameter of a variadic definition");
    return void_type();
  }
  if (args[1]->getKind() != cparser::NK_CAST_EXPR)
    return error("va_arg requires a type name");
  const auto pointer = read_type(static_cast<cparser::CastExprASTNode *>(args[1])->getType(),
                                 context, true);
  if (!pointer.pointer() || !pointer.element)
    return error("va_arg requires a type name");
  const auto type = unqualified(*pointer.element);
  if (!(type.pointer() || (type.integer() && type.bits >= 32) ||
        (type.real() && type.bits == 64)))
    return error("va_arg supports only promoted integer, pointer and double types (not long double)");
  return type;
}

bool modifiable(const CType &type) {
  if (!type.valid() || type.is_const)
    return false;
  return type.scalar();
}

struct Binary {
  Node *lhs = nullptr;
  Node *rhs = nullptr;
};

std::optional<Binary> binary_children(Node *node) {
  if (!present(node))
    return std::nullopt;
  switch (node->getKind()) {
  case cparser::NK_LSHIFT_EXPR: {
    auto *n = static_cast<cparser::LShiftExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_RSHIFT_EXPR: {
    auto *n = static_cast<cparser::RShiftExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_BIT_IOR_EXPR: {
    auto *n = static_cast<cparser::BitIorExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_BIT_XOR_EXPR: {
    auto *n = static_cast<cparser::BitXorExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_BIT_AND_EXPR: {
    auto *n = static_cast<cparser::BitAndExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_LOG_AND_EXPR: {
    auto *n = static_cast<cparser::LogAndExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_LOG_OR_EXPR: {
    auto *n = static_cast<cparser::LogOrExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_PLUS_EXPR: {
    auto *n = static_cast<cparser::PlusExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_MINUS_EXPR: {
    auto *n = static_cast<cparser::MinusExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_MULT_EXPR: {
    auto *n = static_cast<cparser::MultExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_TRUNC_DIV_EXPR: {
    auto *n = static_cast<cparser::TruncDivExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_TRUNC_MOD_EXPR: {
    auto *n = static_cast<cparser::TruncModExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_LT_EXPR: {
    auto *n = static_cast<cparser::LtExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_LE_EXPR: {
    auto *n = static_cast<cparser::LeExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_GT_EXPR: {
    auto *n = static_cast<cparser::GtExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_GE_EXPR: {
    auto *n = static_cast<cparser::GeExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_EQ_EXPR: {
    auto *n = static_cast<cparser::EqExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  case cparser::NK_NE_EXPR: {
    auto *n = static_cast<cparser::NeExprASTNode *>(node);
    return Binary{n->getLhs(), n->getRhs()};
  }
  default:
    return std::nullopt;
  }
}

bool is_null_pointer_constant(const TypeContext &context, Node *node,
                              const CType &type) {
  if (!present(node))
    return false;
  if (is_unqualified_void_pointer(type) && node->getKind() == cparser::NK_GENERIC_EXPR)
    node = select_generic(node, context);
  if (!present(node))
    return false;
  if (is_unqualified_void_pointer(type) &&
      node->getKind() == cparser::NK_CAST_EXPR)
    node = static_cast<cparser::CastExprASTNode *>(node)->getExpr();
  else if (!type.integer())
    return false;
  // Probing a runtime expression for an ICE must not diagnose evaluation of
  // an expression that could be short-circuited or otherwise unevaluated.
  Diagnostics probe;
  const auto value = integer_constant(node, context.with_probe(probe));
  return value && value->value == 0;
}

bool assignment_compatible(const TypeContext &context, const CType &target,
                           const CType &source, Node *node) {
  if (target.kind == CType::Kind::record)
    return size_of(target) && compatible(unqualified(target), unqualified(source));
  if (target.arithmetic())
    return source.arithmetic() || (target.integer() && target.bits == 1 && source.pointer());
  if (!target.pointer())
    return false;
  if (is_null_pointer_constant(context, node, source))
    return true;
  if (!source.pointer())
    return false;
  if (target.function_pointer() || source.function_pointer())
    return compatible(unqualified(target), source);
  return compatible_object_pointers(target, source) &&
         !drops_qualifiers(source, target);
}

CType arithmetic_type(ASTNodeKind kind, const CType &lhs, const CType &rhs) {
  if (kind == cparser::NK_PLUS_EXPR) {
    if (complete_object_pointer(lhs) && rhs.integer())
      return lhs;
    if (complete_object_pointer(rhs) && lhs.integer())
      return rhs;
  } else if (kind == cparser::NK_MINUS_EXPR) {
    if (complete_object_pointer(lhs) && rhs.integer())
      return lhs;
    if (complete_object_pointer(lhs) && complete_object_pointer(rhs) &&
        compatible_object_pointers(lhs, rhs))
      return difference_type();
  }
  if (kind == cparser::NK_PLUS_EXPR || kind == cparser::NK_MINUS_EXPR ||
      kind == cparser::NK_MULT_EXPR || kind == cparser::NK_TRUNC_DIV_EXPR)
    return common_arithmetic(lhs, rhs);
  if (!lhs.integer() || !rhs.integer())
    return invalid_type();
  if (kind == cparser::NK_LSHIFT_EXPR || kind == cparser::NK_RSHIFT_EXPR)
    return promote(lhs);
  return common_integer(lhs, rhs);
}

CType conditional_type(const TypeContext &context, Node *then_node,
                       Node *else_node) {
  const auto lhs = decay(expression_type(then_node, context));
  const auto rhs = decay(expression_type(else_node, context));
  if (!lhs.valid() || !rhs.valid())
    return invalid_type();
  if (lhs.arithmetic() && rhs.arithmetic())
    return common_arithmetic(lhs, rhs);
  if (is_void(lhs) && is_void(rhs))
    return void_type();
  if (lhs.kind == CType::Kind::record && size_of(lhs) && compatible(lhs, rhs))
    return lhs;
  if (lhs.pointer() && is_null_pointer_constant(context, else_node, rhs))
    return lhs;
  if (rhs.pointer() && is_null_pointer_constant(context, then_node, lhs))
    return rhs;
  if (lhs.pointer() && rhs.pointer()) {
    if (lhs.function_pointer() || rhs.function_pointer()) {
      if (!compatible(lhs, rhs))
        return invalid_type();
      return rhs.element->prototype ? rhs : lhs;
    }
    return compatible_object_pointers(lhs, rhs) ? pointer_composite(lhs, rhs)
                                               : invalid_type();
  }
  return invalid_type();
}

Node *lvalue_base(Node *node, const TypeContext &context) {
  while (present(node)) {
    if (node->getKind() == cparser::NK_STRUCT_REF)
      node = static_cast<cparser::StructRefASTNode *>(node)->getName();
    else if (node->getKind() == cparser::NK_GENERIC_EXPR)
      node = select_generic(node, context);
    else
      break;
  }
  return node;
}

bool describes_lvalue(Node *node, const TypeContext &context) {
  // Callers have already checked the type and (for indirection) the operand.
  node = lvalue_base(node, context);
  if (!present(node))
    return false;
  switch (node->getKind()) {
  case cparser::NK_IDENT_NODE:
  case cparser::NK_STRING_CONST:
  case cparser::NK_COMPOUND_LITERAL:
  case cparser::NK_INDIRECT_REF:
  case cparser::NK_ARRAY_REF:
    return true;
  default:
    return false;
  }
}

bool type_modifiable(Node *node, const CType &type, const TypeContext &context) {
  return (type.scalar() || (type.kind == CType::Kind::record && size_of(type))) &&
         !object_has_qualifier(type, &CType::is_const) && describes_lvalue(node, context);
}

bool register_subobject(Node *node, const TypeContext &context) {
  node = lvalue_base(node, context);
  return present(node) && node->getKind() == cparser::NK_IDENT_NODE &&
         context.lookup(identifier(node)).register_object;
}

CType member_type(const CType &record, Node *name, const TypeContext &context) {
  if (!record.valid())
    return {};
  if (const auto member = find_member(record, identifier(name)))
    return member->type;
  const auto message = record.kind != CType::Kind::record
      ? "member access requires a structure or union operand"
      : !size_of(record) ? "member access requires a complete structure or union type"
                         : "no such member '" + identifier(name) + "'";
  context.diagnostics.error("cephyr.record.member", message, location(context.source, name));
  return {};
}

thread_local unsigned type_expression_depth = 0;

} // namespace

std::optional<std::vector<ObjectInitEntry>> plan_compound_literal(
    const TypeContext &context, Node *node, CType &type, bool evaluated) {
  auto *literal = static_cast<cparser::CompoundLiteralASTNode *>(node);
  type = read_type(literal->getType(), context, true);
  const auto error = [&](Node *at, std::string_view message) {
    context.diagnostics.error("cephyr.compound-literal", message, location(context.source, at));
    return std::nullopt;
  };
  if (!type.valid())
    return std::nullopt;
  if (!size_of(type) && (type.kind != CType::Kind::array || type.count))
    return error(node, "compound literal requires a complete object type or array of unknown size");
  auto plan = plan_object_initializer(
      context, type, literal->getInit(),
      evaluated && context.function && object_has_qualifier(type, &CType::is_volatile));
  if (!plan)
    return std::nullopt;
  if (!size_of(type))
    return error(node, "compound literal has incomplete or unsupported object size");
  // Check initializer constraints even without evaluating the literal (e.g.
  // as the operand of sizeof or an unselected generic association).
  for (const auto &entry : *plan) {
    if (entry.clear || entry.bytes)
      continue;
    auto *value = entry.source;
    while (value->getKind() == cparser::NK_LIST) {
      const auto values = elements(value);
      if (values.size() != 1)
        return error(value, "scalar initializer must contain exactly one expression");
      value = values.front();
    }
    if (value->getKind() == cparser::NK_DESIGNATED_INIT)
      return error(value, "scalar initializer cannot have a designator");
    if (!context.function || evaluated)
      continue; // The static or runtime initializer will check the value.
    const auto errors = context.diagnostics.error_count();
    const auto source = decay(expression_type(value, context));
    if (!assignment_compatible(context, entry.type, source, value)) {
      if (context.diagnostics.error_count() != errors)
        return std::nullopt;
      return error(value, "incompatible compound literal initializer");
    }
  }
  return plan;
}

CType expression_type(Node *node, const TypeContext &context) {
  const AnalysisDepth depth(type_expression_depth);
  if (!present(node))
    return invalid_type();
  const auto type_of = [&](Node *child) { return expression_type(child, context); };
  const auto as_value = [&](Node *child) { return decay(type_of(child)); };
  const auto increment_type = [&](Node *child, bool post = false) {
    auto type = type_of(child);
    type.denotes_bit_field = false;
    if (post)
      type.bit_field_width = 0;
    return type_modifiable(child, type, context) &&
                   (type.arithmetic() || complete_object_pointer(type))
               ? unqualified(type) : invalid_type();
  };
  const auto comma_type = [&](Node *list) {
    CType result;
    for (auto *child : elements(list)) {
      result = as_value(child);
      if (!result.valid())
        return invalid_type();
    }
    result.denotes_bit_field = false;
    return result;
  };

  switch (node->getKind()) {
  case cparser::NK_GENERIC_EXPR: {
    CType result;
    return select_generic(node, context, &result) ? result : invalid_type();
  }
  case cparser::NK_INTEGER_CONST:
  case cparser::NK_CHAR_CONST: {
    auto value = integer_constant(node, context);
    return value ? value->type : invalid_type();
  }
  case cparser::NK_REAL_CONST: {
    const auto value = real_constant(node, context);
    return value ? value->type : invalid_type();
  }
  case cparser::NK_STRING_CONST:
    return string_constant_type(node, context.source);
  case cparser::NK_COMPOUND_LITERAL: {
    CType type;
    auto plan = plan_compound_literal(context, node, type);
    return plan && (context.function || validate_static_compound_initializer(context, *plan))
               ? type : invalid_type();
  }
  case cparser::NK_IDENT_NODE: {
    const auto name = identifier(node);
    auto type = context.lookup(name, node).type;
    if (!type.valid())
      context.diagnostics.error("cephyr.type", "no valid declaration for '" + name + "'",
                                location(context.source, node));
    return type;
  }
  case cparser::NK_CAST_EXPR: {
    auto *cast = static_cast<cparser::CastExprASTNode *>(node);
    auto target = unqualified(read_type(cast->getType(), context, true));
    if (!target.valid() || !present(cast->getExpr()))
      return invalid_type();
    if (target.integer() && cast->getExpr()->getKind() == cparser::NK_REAL_CONST)
      // In an unevaluated context, check the literal format but not whether
      // its value fits the cast's integer result. No runtime FP ABI is needed.
      return real_constant(cast->getExpr(), context, false) ? target : invalid_type();
    const auto source = as_value(cast->getExpr());
    if (!source.valid() || (!is_void(target) && (!target.scalar() || !source.scalar())))
      return invalid_type();
    if ((target.real() && source.pointer()) || (target.pointer() && source.real()))
      return invalid_type();
    if (target.pointer() && source.pointer() &&
        target.function_pointer() != source.function_pointer() &&
        !(target.function_pointer() && is_null_pointer_constant(context, cast->getExpr(), source)))
      return invalid_type();
    return target;
  }
  case cparser::NK_BIT_NOT_EXPR:
    return promote(as_value(static_cast<cparser::BitNotExprASTNode *>(node)->getExpr()));
  case cparser::NK_UNARY_PLUS_EXPR: {
    const auto operand = as_value(static_cast<cparser::UnaryPlusASTNode *>(node)->getExpr());
    return operand.real() ? operand : promote(operand);
  }
  case cparser::NK_LOG_NOT_EXPR:
    return as_value(static_cast<cparser::LogNotExprASTNode *>(node)->getExpr()).scalar()
               ? int_type() : invalid_type();
  case cparser::NK_PREDECREMENT_EXPR:
    return increment_type(static_cast<cparser::PredecrementExprASTNode *>(node)->getExpr());
  case cparser::NK_PREINCREMENT_EXPR:
    return increment_type(static_cast<cparser::PreincrementExprASTNode *>(node)->getExpr());
  case cparser::NK_POSTDECREMENT_EXPR:
    return increment_type(static_cast<cparser::PostdecrementExprASTNode *>(node)->getExpr(), true);
  case cparser::NK_POSTINCREMENT_EXPR:
    return increment_type(static_cast<cparser::PostincrementExprASTNode *>(node)->getExpr(), true);
  case cparser::NK_ADDR_EXPR: {
    auto *operand = static_cast<cparser::AddrExprASTNode *>(node)->getExpr();
    auto type = type_of(operand);
    if (type.denotes_bit_field) {
      context.diagnostics.error("cephyr.type", "cannot take the address of a bit-field",
                                location(context.source, operand));
      return invalid_type();
    }
    if (type.valid() && register_subobject(operand, context)) {
      context.diagnostics.error("cephyr.type", "cannot take the address of a register object",
                                location(context.source, operand));
      return invalid_type();
    }
    return type.valid() && describes_lvalue(operand, context) ? pointer_type(type) : invalid_type();
  }
  case cparser::NK_INDIRECT_REF: {
    auto *indirect = static_cast<cparser::IndirectRefASTNode *>(node);
    auto type = as_value(indirect->getExpr());
    if (present(indirect->getField())) {
      if (type.pointer() && type.element)
        return member_type(*type.element, indirect->getField(), context);
      if (type.valid())
        context.diagnostics.error("cephyr.record.member", "'->' requires a pointer to a structure or union",
                                  location(context.source, indirect->getField()));
      return invalid_type();
    }
    return type.pointer() && type.element ? *type.element : invalid_type();
  }
  case cparser::NK_STRUCT_REF: {
    auto *member = static_cast<cparser::StructRefASTNode *>(node);
    return member_type(type_of(member->getName()), member->getMember(), context);
  }
  case cparser::NK_ARRAY_REF: {
    auto *array = static_cast<cparser::ArrayRefASTNode *>(node);
    auto base = as_value(array->getExpr());
    auto index = as_value(array->getIndex());
    if (base.integer() && index.pointer())
      std::swap(base, index);
    return complete_object_pointer(base) && index.integer() ? *base.element
                                                            : invalid_type();
  }
  case cparser::NK_ASSIGN_EXPR: {
    auto *assign = static_cast<cparser::AssignExprASTNode *>(node);
    auto target = type_of(assign->getLhs());
    if (!type_modifiable(assign->getLhs(), target, context))
      return invalid_type();
    const auto source = as_value(assign->getRhs());
    target.denotes_bit_field = false;
    return assignment_compatible(context, target, source, assign->getRhs())
               ? unqualified(target) : invalid_type();
  }
  case cparser::NK_COND_EXPR: {
    auto *conditional = static_cast<cparser::CondExprASTNode *>(node);
    return as_value(conditional->getCondition()).scalar()
               ? conditional_type(context, conditional->getThenClause(),
                                  conditional->getElseClause())
               : invalid_type();
  }
  case cparser::NK_CALL_EXPR: {
    auto *call = static_cast<cparser::CallExprASTNode *>(node);
    if (present(call->getExpr()) && call->getExpr()->getKind() == cparser::NK_IDENT_NODE) {
      const auto name = identifier(call->getExpr());
      if (stdarg_builtin(name))
        return stdarg_builtin_type(context, node, name, elements(call->getArgs()));
    }
    const auto callee = as_value(call->getExpr());
    if (!callee.function_pointer() || !callee.element->element)
      return invalid_type();
    const auto &signature = *callee.element;
    if (signature.variadic && (!scalar_variadic_signature(signature) ||
        (!context.source.native_scalar_stdarg() &&
         (signature.element->real() ||
          std::any_of(signature.params.begin(), signature.params.end(),
                      [](const CType &type) { return type.real(); })))))
      return invalid_type();
    const auto args = elements(call->getArgs());
    if (signature.prototype && (args.size() < signature.params.size() ||
        (!signature.variadic && args.size() != signature.params.size())))
      return invalid_type();
    for (std::size_t i = 0; i < args.size(); ++i) {
      const auto argument = as_value(args[i]);
      if (i < signature.params.size()) {
        if (!assignment_compatible(context, signature.params[i], argument, args[i]))
          return invalid_type();
      } else if (!argument.scalar() || (argument.real() && !context.source.native_scalar_stdarg())) {
        return invalid_type();
      }
    }
    return *signature.element;
  }
  case cparser::NK_SIZEOF_EXPR:
  case cparser::NK_ALIGNOF_EXPR:
  case cparser::NK_OFFSETOF_EXPR: {
    const auto size = layout_constant(node, context);
    return size ? size->type : invalid_type();
  }
  case cparser::NK_LIST:
    return comma_type(node);
  case cparser::NK_COMPOUND_EXPR:
    return comma_type(static_cast<cparser::CompoundExprASTNode *>(node)->getElements());
  case cparser::NK_NOP_EXPR:
    return void_type();
  default:
    break;
  }
  const auto children = binary_children(node);
  if (!children)
    return invalid_type();
  const auto lhs = as_value(children->lhs);
  const auto rhs = as_value(children->rhs);
  switch (node->getKind()) {
  case cparser::NK_LOG_AND_EXPR:
  case cparser::NK_LOG_OR_EXPR:
    return lhs.scalar() && rhs.scalar() ? int_type() : invalid_type();
  case cparser::NK_EQ_EXPR:
  case cparser::NK_NE_EXPR:
    if ((lhs.pointer() && is_null_pointer_constant(context, children->rhs, rhs)) ||
        (rhs.pointer() && is_null_pointer_constant(context, children->lhs, lhs)))
      return int_type();
    if (lhs.function_pointer() && rhs.function_pointer() && compatible(lhs, rhs))
      return int_type();
    if (compatible_object_pointers(lhs, rhs))
      return int_type();
    return lhs.arithmetic() && rhs.arithmetic() ? int_type() : invalid_type();
  case cparser::NK_LT_EXPR:
  case cparser::NK_LE_EXPR:
  case cparser::NK_GT_EXPR:
  case cparser::NK_GE_EXPR:
    return ((lhs.arithmetic() && rhs.arithmetic()) ||
            (is_object_pointer(lhs) && is_object_pointer(rhs) &&
             compatible_object_pointers(lhs, rhs))) ? int_type() : invalid_type();
  default:
    return arithmetic_type(node->getKind(), lhs, rhs);
  }
}

namespace {

Expr narrow_bit_field(Function &function, Expr value, const LValue &target) {
  if (!target.bit_field || !value.type.valid())
    return value;
  auto type = unqualified(target.type);
  auto width = target.bit_field->width;
  return {type, function.emit(type, type.is_signed ? "sext" : "zext",
                              id(value.id) + " " + std::to_string(width)), std::nullopt};
}

bool uses_bit_field_snapshot(const LValue &target) {
  // AAPCS64 expressly forbids merging the value read and the write's
  // preservation read (e.g., increment is two reads and one write).
  return target.type.is_volatile && target.bit_field &&
         target.bit_field->volatile_access == BitField::VolatileAccess::narrow;
}

struct VolatileBitFieldAccess {
  // C11 5.1.2.3p6 and 6.7.3p7: use the target's volatile access policy.
  // AAPCS64 mandates a container-width load and a load/store pair for a
  // write, even when every bit is replaced. AMD64/RV64 use only the field's
  // occupied bytes, with the widest naturally aligned chunks, low first.
  explicit VolatileBitFieldAccess(const LValue &target)
      : container(target.bit_field->volatile_access == BitField::VolatileAccess::container),
        bytes(container ? align_of(target.type) : target.bit_field->bytes()),
        shift(target.bit_field->shift + (container ? 8 * target.bit_field->byte_in_container : 0)),
        displacement(container ? 0 : target.bit_field->byte_in_container),
        mask(target.bit_field->mask() << shift) {}

  bool container;
  unsigned bytes, shift, displacement;
  std::uint64_t mask;

  static std::uint64_t byte_mask(unsigned bytes) {
    return (std::uint64_t{1} << (bytes * 8)) - 1;
  }

  template <class Step> void for_each_chunk(Step step) const {
    for (unsigned i = 0; i < bytes;) {
      auto width = bytes;
      if (!container) {
        width = 1;
        while (2 * width <= bytes - i && (displacement + i) % (2 * width) == 0)
          width *= 2;
      }
      step(width, displacement + i, i * 8);
      i += width;
    }
  }
};

Id read_bit_field_storage(Function &function, const LValue &target, Node *at,
                         const VolatileBitFieldAccess &access, bool preserve_only) {
  const auto word = integer_type(64, false);
  Id storage = no_value;
  access.for_each_chunk([&](unsigned width, unsigned disp, unsigned shift) {
    const auto mask = VolatileBitFieldAccess::byte_mask(width);
    if (preserve_only && !access.container && ((access.mask >> shift) & mask) == mask)
      return;
    const auto *op = width == 4 ? "load_u32" : width == 2 ? "load_u16" : "load_u8";
    auto part = function.emit(word, op, id(target.address) + " " + std::to_string(disp), at);
    if (shift)
      part = function.emit(word, "shl", binary_operands(part, function.constant(shift, word)));
    storage = storage == no_value ? part : function.emit(word, "or", binary_operands(storage, part));
  });
  return storage;
}

bool supported_volatile_bit_field(Function &function, const LValue &target, Node *at) {
  if (target.bit_field->volatile_access != BitField::VolatileAccess::unsupported)
    return true;
  function.error(at, "volatile bit-field access to an overlapping container or union member is not supported");
  return false;
}

Expr read_volatile_bit_field(Function &function, const LValue &target, Node *at,
                             Id *snapshot = nullptr) {
  if (!supported_volatile_bit_field(function, target, at))
    return invalid_expr();
  const auto word = integer_type(64, false);
  const VolatileBitFieldAccess access(target);
  auto storage = read_bit_field_storage(function, target, at, access, false);
  if (snapshot)
    *snapshot = storage;
  if (access.shift)
    storage = function.emit(word, "lshr", binary_operands(storage, function.constant(access.shift, word)));
  return narrow_bit_field(function, {word, storage, std::nullopt}, target);
}

void write_volatile_bit_field(Function &function, const LValue &target, Expr value,
                              Node *at, Id snapshot = no_value) {
  if (!supported_volatile_bit_field(function, target, at))
    return;
  const auto word = integer_type(64, false);
  const VolatileBitFieldAccess access(target);
  auto bits = function.emit(word, "and",
      binary_operands(value.id, function.constant(target.bit_field->mask(), word)));
  if (access.shift)
    bits = function.emit(word, "shl", binary_operands(bits, function.constant(access.shift, word)));
  if (snapshot == no_value)
    snapshot = read_bit_field_storage(function, target, at, access, true);
  const auto keep = VolatileBitFieldAccess::byte_mask(access.bytes) ^ access.mask;
  if (keep) {
    const auto kept = function.emit(word, "and", binary_operands(snapshot, function.constant(keep, word)));
    bits = function.emit(word, "or", binary_operands(bits, kept));
  }
  access.for_each_chunk([&](unsigned width, unsigned disp, unsigned shift) {
    const auto part = shift ? function.emit(word, "lshr",
        binary_operands(bits, function.constant(shift, word))) : bits;
    const auto *op = width == 4 ? "store32" : width == 2 ? "store16" : "store8";
    function.emit_void(op, binary_operands(target.address, part) + " " + std::to_string(disp));
  });
}

Expr canonicalize(Function &function, Expr value, Node *at) {
  if (!value.type.integer() || value.id == no_value)
    return value;
  if (value.type.bits == 0 || value.type.bits > 64) {
    function.error(at, "unsupported integer width");
    return invalid_expr();
  }
  if (value.type.bits == 64)
    return value;
  const auto operation = value.type.is_signed ? "sext" : "zext";
  value.id = function.emit(value.type, operation,
                           id(value.id) + " " + std::to_string(value.type.bits));
  if (value.constant)
    value.constant = normalize(*value.constant, value.type);
  return value;
}

Id invert(Function &function, Id condition) {
  const auto zero = function.constant(0, int_type());
  return function.emit(int_type(), "eq", binary_operands(condition, zero));
}

Id floating_bits(Function &function, const CType &type, std::uint64_t bits) {
  return function.emit(type, "fconst", std::to_string(bits));
}

Expr x87_constant(Function &function, Constant value) {
  return {value.type, function.constant(value.value, size_type()), std::nullopt,
          function.constant(value.x87_sign_exponent, integer_type(16, false))};
}

// The scalar snapshot stays in two SSA values, even across side effects
// and control-flow edges. Materialize only for AMD64 instructions that
// consume the 10-byte value in memory; never give them the source lvalue.
LValue x87_temporary(Function &function, Expr value, Node *at) {
  auto temporary = function.local(unqualified(value.type), at);
  store(function, temporary, value, at);
  return temporary;
}

Id x87_compare(Function &function, const char *op, Expr lhs, Expr rhs, Node *at) {
  const auto left = x87_temporary(function, lhs, at);
  const auto right = x87_temporary(function, rhs, at);
  if (left.address == no_value || right.address == no_value)
    return no_value;
  return function.emit(int_type(), op, binary_operands(left.address, right.address), at);
}

Expr pointer_offset(Function &function, Expr pointer, Expr index, bool subtract,
                    Node *at) {
  if (!complete_object_pointer(pointer.type)) {
    function.error(at, "pointer arithmetic requires a complete object type");
    return invalid_expr();
  }
  if (!index.type.integer()) {
    function.error(at, "pointer arithmetic requires an integer offset");
    return invalid_expr();
  }
  const auto promoted_index = promote(index.type);
  index = convert(function, index, promoted_index, at);
  if (!index.type.valid())
    return invalid_expr();
  // Scaling is address computation, not arithmetic in the promoted C integer
  // type. Widen first so a valid large 32-bit subscript is not truncated after
  // multiplication by the element size.
  const auto index_type =
      integer_type(64, promoted_index.is_signed, promoted_index.rank);
  index = convert(function, index, index_type, at);
  if (!index.type.valid())
    return invalid_expr();
  const auto stride = *size_of(*pointer.type.element);
  if (stride != 1) {
    auto amount = function.constant(stride, index_type);
    index.id =
        function.emit(index_type, "mul", binary_operands(index.id, amount));
    index = canonicalize(function, index, at);
  }
  if (subtract) {
    auto zero = function.constant(0, index_type);
    index.id =
        function.emit(index_type, "sub", binary_operands(zero, index.id));
    index = canonicalize(function, index, at);
  }
  pointer.id = function.emit(pointer.type, "ptr_add",
                             binary_operands(pointer.id, index.id));
  pointer.constant.reset();
  return pointer;
}

Expr pointer_difference(Function &function, Expr lhs, Expr rhs, Node *at) {
  if (!complete_object_pointer(lhs.type) ||
      !compatible_object_pointers(lhs.type, rhs.type) ||
      is_void_pointer(lhs.type) || is_void_pointer(rhs.type)) {
    function.error(at, "pointer subtraction requires compatible complete object "
                       "pointer types");
    return invalid_expr();
  }
  const auto common_pointer = pointer_composite(lhs.type, rhs.type);
  lhs = convert(function, lhs, common_pointer, at);
  rhs = convert(function, rhs, common_pointer, at);
  auto lhs_int =
      function.emit(difference_type(), "ptr_to_int", id(lhs.id));
  auto rhs_int =
      function.emit(difference_type(), "ptr_to_int", id(rhs.id));
  Expr result{difference_type(),
              function.emit(difference_type(), "sub",
                            binary_operands(lhs_int, rhs_int)),
              std::nullopt};
  const auto stride = *size_of(*lhs.type.element);
  if (stride != 1) {
    const auto amount = function.constant(stride, difference_type());
    result.id = function.emit(difference_type(), "div_s",
                              binary_operands(result.id, amount));
  }
  return result;
}

Expr integer_binary(Function &function, ASTNodeKind kind, Expr lhs, Expr rhs,
                    Node *at) {
  if (!lhs.type.integer() || !rhs.type.integer()) {
    function.error(at, "operator requires integer operands");
    return invalid_expr();
  }

  if (kind == cparser::NK_LSHIFT_EXPR ||
      kind == cparser::NK_RSHIFT_EXPR) {
    const auto result_type = promote(lhs.type);
    lhs = convert(function, lhs, result_type, at);
    rhs = convert(function, rhs, promote(rhs.type), at);
    if (!lhs.type.valid() || !rhs.type.valid())
      return invalid_expr();
    const char *operation =
        kind == cparser::NK_LSHIFT_EXPR
            ? "shl"
            : (result_type.is_signed ? "ashr" : "lshr");
    Expr result{result_type,
                function.emit(result_type, operation,
                              binary_operands(lhs.id, rhs.id)),
                std::nullopt};
    return canonicalize(function, result, at);
  }

  const auto result_type =
      common_integer(promote(lhs.type), promote(rhs.type));
  if (!result_type.valid()) {
    function.error(at, "cannot determine the common integer type");
    return invalid_expr();
  }
  lhs = convert(function, lhs, result_type, at);
  rhs = convert(function, rhs, result_type, at);
  if (!lhs.type.valid() || !rhs.type.valid())
    return invalid_expr();

  const char *operation = nullptr;
  switch (kind) {
  case cparser::NK_PLUS_EXPR:
    operation = "add";
    break;
  case cparser::NK_MINUS_EXPR:
    operation = "sub";
    break;
  case cparser::NK_MULT_EXPR:
    operation = "mul";
    break;
  case cparser::NK_TRUNC_DIV_EXPR:
    operation = result_type.is_signed ? "div_s" : "div_u";
    break;
  case cparser::NK_TRUNC_MOD_EXPR:
    operation = result_type.is_signed ? "rem_s" : "rem_u";
    break;
  case cparser::NK_BIT_AND_EXPR:
    operation = "and";
    break;
  case cparser::NK_BIT_XOR_EXPR:
    operation = "xor";
    break;
  case cparser::NK_BIT_IOR_EXPR:
    operation = "or";
    break;
  default:
    function.error(at, "unsupported integer operator");
    return invalid_expr();
  }
  Expr result{result_type,
              function.emit(result_type, operation,
                            binary_operands(lhs.id, rhs.id)),
              std::nullopt};
  return canonicalize(function, result, at);
}

// Use ordinary typed imports and LP64 calls: the Archon RV64 provider
// already transports f32/f64 through the integer argument and result slots.
// The helper signatures use XLEN integers, never C's narrow ABI slots.
Expr rv64_soft_fp_call(Function &function, std::string name, const CType &result,
                      Expr a, Node *at, std::optional<Expr> b = std::nullopt) {
  CType signature;
  signature.kind = CType::Kind::function;
  signature.element = std::make_shared<CType>(result);
  signature.prototype = true;
  signature.params.push_back(a.type.integer() ? integer_type(64, a.type.is_signed, 4) : a.type);
  std::string operands = name + (b ? " 2 " : " 1 ") + id(a.id);
  if (b) {
    signature.params.push_back(b->type);
    operands += " " + id(b->id);
  }
  function.unit.rv64_soft_fp_helpers.try_emplace(std::move(name), std::move(signature));
  return {result, function.emit(result, "call", operands, at), std::nullopt};
}

Expr arithmetic_binary(Function &function, ASTNodeKind kind, Expr lhs, Expr rhs,
                       Node *at) {
  if (kind == cparser::NK_PLUS_EXPR) {
    if (lhs.type.pointer() && rhs.type.integer())
      return pointer_offset(function, lhs, rhs, false, at);
    if (rhs.type.pointer() && lhs.type.integer())
      return pointer_offset(function, rhs, lhs, false, at);
  } else if (kind == cparser::NK_MINUS_EXPR) {
    if (lhs.type.pointer() && rhs.type.integer())
      return pointer_offset(function, lhs, rhs, true, at);
    if (lhs.type.pointer() && rhs.type.pointer())
      return pointer_difference(function, lhs, rhs, at);
  }
  if (lhs.type.real() || rhs.type.real()) {
    const auto type = arithmetic_type(kind, lhs.type, rhs.type);
    if (!type.real()) {
      if (lhs.type.valid() && rhs.type.valid())
        function.error(at, "operator does not accept these floating-point operands");
      return invalid_expr();
    }
    lhs = convert(function, lhs, type, at);
    rhs = convert(function, rhs, type, at);
    if (!lhs.type.valid() || !rhs.type.valid())
      return invalid_expr();
    if (type.extended_real()) {
      const auto left = x87_temporary(function, lhs, at);
      const auto right = x87_temporary(function, rhs, at);
      if (left.address == no_value || right.address == no_value)
        return invalid_expr();
      const char *operation = kind == cparser::NK_PLUS_EXPR ? "amd64_x87_add" :
          kind == cparser::NK_MINUS_EXPR ? "amd64_x87_sub" :
          kind == cparser::NK_MULT_EXPR ? "amd64_x87_mul" : "amd64_x87_div";
      function.emit_void(operation, binary_operands(left.address, right.address));
      return load(function, left, at);
    }
    if (function.unit.source.native_rv64_soft_fp) {
      const char *stem = kind == cparser::NK_PLUS_EXPR ? "__add" :
          kind == cparser::NK_MINUS_EXPR ? "__sub" :
          kind == cparser::NK_MULT_EXPR ? "__mul" : "__div";
      return rv64_soft_fp_call(function, std::string(stem) + (type.bits == 32 ? "sf3" : "df3"),
                             type, lhs, at, rhs);
    }
    const char *operation = kind == cparser::NK_PLUS_EXPR ? "fadd" :
        kind == cparser::NK_MINUS_EXPR ? "fsub" :
        kind == cparser::NK_MULT_EXPR ? "fmul" : "fdiv";
    return {type, function.emit(type, operation, binary_operands(lhs.id, rhs.id)), std::nullopt};
  }
  return integer_binary(function, kind, lhs, rhs, at);
}

std::optional<std::pair<Expr, Expr>>
comparison_operands(Function &function, Expr lhs, Expr rhs, Node *at,
                    bool relational) {
  if (lhs.type.arithmetic() && rhs.type.arithmetic()) {
    const auto type = common_arithmetic(lhs.type, rhs.type);
    lhs = convert(function, lhs, type, at);
    rhs = convert(function, rhs, type, at);
    return std::pair{lhs, rhs};
  }

  if (!relational) {
    if (lhs.type.pointer() && is_null_pointer_constant(rhs))
      rhs = convert(function, rhs, lhs.type, at);
    else if (rhs.type.pointer() && is_null_pointer_constant(lhs))
      lhs = convert(function, lhs, rhs.type, at);
  }

  if (!lhs.type.pointer() || !rhs.type.pointer()) {
    function.error(at, "comparison requires arithmetic operands or compatible "
                       "pointers");
    return std::nullopt;
  }
  if (lhs.type.function_pointer() || rhs.type.function_pointer()) {
    if (relational || !compatible(lhs.type, rhs.type)) {
      function.error(at, "function pointers support only equality comparisons "
                         "with compatible function pointers or null pointer constants");
      return std::nullopt;
    }
  } else if (!compatible_object_pointers(lhs.type, rhs.type)) {
    function.error(at, "comparison of incompatible pointer types");
    return std::nullopt;
  }
  if (relational &&
      (is_void_pointer(lhs.type) || is_void_pointer(rhs.type))) {
    function.error(at, "relational comparison requires object pointers");
    return std::nullopt;
  }
  const auto cast = lhs.type.function_pointer() ? "func_ptr_to_int" : "ptr_to_int";
  const auto bits = size_type();
  return std::pair{
      Expr{bits, function.emit(bits, cast, id(lhs.id)), std::nullopt},
      Expr{bits, function.emit(bits, cast, id(rhs.id)), std::nullopt}};
}

Expr comparison(Function &function, ASTNodeKind kind, Expr lhs, Expr rhs,
                Node *at) {
  const bool relational =
      kind == cparser::NK_LT_EXPR || kind == cparser::NK_LE_EXPR ||
      kind == cparser::NK_GT_EXPR || kind == cparser::NK_GE_EXPR;
  auto operands =
      comparison_operands(function, lhs, rhs, at, relational);
  if (!operands)
    return invalid_expr();
  lhs = operands->first;
  rhs = operands->second;
  if (!lhs.type.valid() || !rhs.type.valid())
    return invalid_expr();

  Id result = no_value;
  if (lhs.type.real()) {
    const bool inclusive = kind == cparser::NK_LE_EXPR || kind == cparser::NK_GE_EXPR;
    if (kind == cparser::NK_GT_EXPR || kind == cparser::NK_GE_EXPR)
      std::swap(lhs, rhs);
    if (lhs.type.extended_real()) {
      const char *op = !relational ? "amd64_x87_eq" : inclusive ? "amd64_x87_le" : "amd64_x87_lt";
      result = x87_compare(function, op, lhs, rhs, at);
      if (result == no_value)
        return invalid_expr();
    } else {
      const char *operation = !relational ? "feq" : inclusive ? "fle" : "flt";
      result = function.emit(int_type(), operation, binary_operands(lhs.id, rhs.id));
    }
    if (kind == cparser::NK_NE_EXPR)
      result = invert(function, result);
    return {int_type(), result, std::nullopt};
  }
  if (kind == cparser::NK_EQ_EXPR || kind == cparser::NK_NE_EXPR) {
    result = function.emit(int_type(), "eq",
                           binary_operands(lhs.id, rhs.id));
    if (kind == cparser::NK_NE_EXPR)
      result = invert(function, result);
    return {int_type(), result, std::nullopt};
  }

  const bool signed_comparison = lhs.type.integer() && lhs.type.is_signed;
  const char *operation = signed_comparison ? "slt" : "ult";
  if (kind == cparser::NK_GT_EXPR || kind == cparser::NK_LE_EXPR)
    std::swap(lhs, rhs);
  result =
      function.emit(int_type(), operation, binary_operands(lhs.id, rhs.id));
  if (kind == cparser::NK_LE_EXPR || kind == cparser::NK_GE_EXPR)
    result = invert(function, result);
  return {int_type(), result, std::nullopt};
}

Expr logical(Function &function, ASTNodeKind kind, Node *lhs_node,
             Node *rhs_node) {
  auto lhs = lower_expr(function, lhs_node);
  const auto condition = truth(function, lhs, lhs_node);
  if (condition == no_value)
    return invalid_expr();

  const auto rhs_block = function.block();
  const auto join = function.block();
  const auto result = function.param(join, int_type());
  const auto fixed =
      function.constant(kind == cparser::NK_LOG_OR_EXPR ? 1 : 0, int_type());
  if (kind == cparser::NK_LOG_AND_EXPR)
    function.branch(condition, rhs_block, join, {}, {fixed});
  else
    function.branch(condition, join, rhs_block, {fixed}, {});

  function.select(rhs_block);
  auto rhs = lower_expr(function, rhs_node);
  const auto rhs_condition = truth(function, rhs, rhs_node);
  if (function.open() && rhs_condition != no_value)
    function.jump(join, {rhs_condition});
  else if (function.open())
    function.trap();
  function.select(join);
  if (rhs_condition == no_value)
    return invalid_expr();
  return {int_type(), result, std::nullopt};
}

Expr conditional(Function &function, cparser::CondExprASTNode *node) {
  // The condition can introduce an enumerator in a type name that is
  // visible in either arm, even though the arms are typed before lowering.
  auto condition = lower_expr(function, node->getCondition());
  const auto condition_id = truth(function, condition, node->getCondition());
  if (condition_id == no_value)
    return invalid_expr();

  const auto result_type =
      conditional_type(TypeContext(function.unit, &function),
                       node->getThenClause(), node->getElseClause());
  if (!result_type.valid()) {
    function.error(node, "incompatible operands to conditional expression");
    return invalid_expr();
  }
  const auto yes = function.block();
  const auto no = function.block();
  const auto join = function.block();
  Id result = no_value;
  Id sign_exponent = no_value;
  if (result_type.extended_real()) {
    result = function.param(join, size_type());
    sign_exponent = function.param(join, integer_type(16, false));
  } else if (!is_void(result_type)) {
    result = function.param(join, result_type.kind == CType::Kind::record
                                      ? pointer_type(result_type) : result_type);
  }
  const auto arguments = [&](const Expr &value) -> std::vector<Id> {
    if (is_void(result_type))
      return {};
    if (result_type.extended_real())
      return {value.id, value.x87_sign_exponent};
    return {value.id};
  };
  function.branch(condition_id, yes, no);

  function.select(yes);
  auto yes_value = lower_expr(function, node->getThenClause());
  if (!is_void(result_type))
    yes_value =
        convert(function, yes_value, result_type, node->getThenClause());
  if (!yes_value.type.valid())
    function.trap();
  else if (function.open())
    function.jump(join, arguments(yes_value));

  function.select(no);
  auto no_value_expr = lower_expr(function, node->getElseClause());
  if (!is_void(result_type))
    no_value_expr =
        convert(function, no_value_expr, result_type, node->getElseClause());
  if (!no_value_expr.type.valid())
    function.trap();
  else if (function.open())
    function.jump(join, arguments(no_value_expr));

  function.select(join);
  if (!yes_value.type.valid() || !no_value_expr.type.valid())
    return invalid_expr();
  return {result_type, result, std::nullopt, sign_exponent};
}

Expr increment(Function &function, Node *operand, bool decrement, bool post,
               Node *at) {
  auto target = lower_lvalue(function, operand);
  if (!target.type.valid())
    return invalid_expr();
  if (!modifiable(target.type)) {
    function.error(at, "increment or decrement requires a modifiable scalar "
                       "lvalue");
    return invalid_expr();
  }
  Id snapshot = no_value;
  auto old = uses_bit_field_snapshot(target) ? read_volatile_bit_field(function, target, operand, &snapshot)
                                            : load(function, target, operand);
  if (!old.type.valid())
    return invalid_expr();
  Expr updated;
  if (old.type.pointer()) {
    updated = pointer_offset(function, old,
                             {int_type(), function.constant(1, int_type()), 1},
                             decrement, at);
  } else {
    updated = arithmetic_binary(
        function,
        decrement ? cparser::NK_MINUS_EXPR : cparser::NK_PLUS_EXPR, old,
        {int_type(), function.constant(1, int_type()), 1}, at);
  }
  updated = convert(function, updated, target.type, at);
  if (!updated.type.valid())
    return invalid_expr();
  if (snapshot != no_value)
    write_volatile_bit_field(function, target, updated, at, snapshot);
  else
    store(function, target, updated, at);
  auto result = post ? old : narrow_bit_field(function, updated, target);
  result.type.denotes_bit_field = false;
  if (post)
    result.type.bit_field_width = 0;
  return result;
}

Expr assignment(Function &function, cparser::AssignExprASTNode *node) {
  auto target = lower_lvalue(function, node->getLhs());
  if (!target.type.valid())
    return invalid_expr();
  if (target.type.kind == CType::Kind::record) {
    auto value = lower_expr(function, node->getRhs());
    if (!copy_record(function, target, value, node))
      return invalid_expr();
    return {unqualified(target.type), value.id, std::nullopt};
  }
  if (!modifiable(target.type)) {
    function.error(node, "assignment requires a modifiable scalar lvalue");
    return invalid_expr();
  }

  Expr value;
  Id snapshot = no_value;
  const auto compound = binary_children(node->getRhs());
  // The parser represents `a op= b` as `a = (a op b)` and deliberately reuses
  // the exact lhs node. Preserve C's single evaluation of `a`.
  if (compound && compound->lhs == node->getLhs()) {
    Expr lhs, rhs;
    if (uses_bit_field_snapshot(target)) {
      // Take the preservation snapshot after evaluating the RHS: its side
      // effects may have updated another field sharing the same bytes.
      rhs = lower_expr(function, compound->rhs);
      lhs = read_volatile_bit_field(function, target, node->getLhs(), &snapshot);
    } else {
      lhs = load(function, target, node->getLhs());
      rhs = lower_expr(function, compound->rhs);
    }
    value = arithmetic_binary(function, node->getRhs()->getKind(), lhs, rhs,
                              node->getRhs());
  } else {
    value = lower_expr(function, node->getRhs());
  }
  value = convert(function, value, target.type, node->getRhs());
  if (!value.type.valid())
    return invalid_expr();
  if (snapshot != no_value)
    write_volatile_bit_field(function, target, value, node, snapshot);
  else
    store(function, target, value, node);
  value = narrow_bit_field(function, value, target);
  value.type.denotes_bit_field = false;
  value.constant.reset(); // Assignment is not an integer constant expression.
  return value;
}

std::string call_operands(std::string target, const std::vector<Id> &args) {
  std::ostringstream operands;
  operands << target << ' ' << args.size();
  for (const auto argument : args)
    operands << ' ' << argument;
  return operands.str();
}

Expr record_call_result(Function &function, const CType &signature, std::string target,
                        Id callee, std::vector<Id> args, Node *at) {
  const auto &type = *signature.element;
  const auto abi = record_abi(type);
  const auto temporary = function.local(type, at);
  if (temporary.address == no_value)
    return invalid_expr();

  if (abi == RecordABI::pair) {
    // The shim writes two full XLEN words. Keep its buffer separate from
    // the C object's storage, which may have only 9--15 bytes.
    const auto buffer = function.local(record_pair_buffer_type(), at);
    if (buffer.address == no_value)
      return invalid_expr();
    if (callee == no_value)
      callee = function.emit(pointer_type(signature), "func_addr", target, at);
    args.push_back(callee);
    args.push_back(buffer.address);
    const auto thunk = function.unit.require_record_pair_call(signature);
    function.emit_void("call", "- " + call_operands(thunk, args));
    const auto word_type = integer_type(64, false);
    for (unsigned index = 0; index < 2; ++index) {
      const auto address = offset_address(function, buffer.address, 8 * index, word_type);
      const auto word = load(function, {word_type, address}, at);
      unpack_record_abi(function, temporary, word.id, index);
    }
  } else {
    const bool indirect_call = callee != no_value;
    if (indirect_call)
      target = id(callee) + ' ' + function.unit.require_signature(signature);
    const auto opcode = indirect_call ? "call_indirect" : "call";
    if (abi == RecordABI::indirect) {
      args.insert(args.begin(), temporary.address);
      function.emit_void(opcode, "- " + call_operands(target, args));
    } else {
      const Id word = function.emit(integer_type(64, false), opcode, call_operands(target, args), at);
      unpack_record_abi(function, temporary, word);
    }
  }
  return {type, temporary.address, std::nullopt};
}

Expr lower_stdarg_builtin(Function &function, cparser::CallExprASTNode *node,
                          std::string_view name) {
  const auto args = elements(node->getArgs());
  const auto type = stdarg_builtin_type(TypeContext(function.unit, &function), node, name, args);
  if (!type.valid())
    return invalid_expr();
  if (function.unit.source.native_rv64_soft_fp) {
    // Lower the list lvalue, not C's unary &: a register-qualified list
    // is valid, and va_end must not load a potentially indeterminate value.
    const auto cursor = lower_lvalue(function, args[0]);
    if (cursor.address == no_value)
      return invalid_expr();
    if (name == "__cephyr_va_end")
      return {type, no_value, std::nullopt};
    if (name == "__cephyr_va_start") {
      function.emit_void("rv64_va_start", id(cursor.address));
      return {type, no_value, std::nullopt};
    }
    // The prologue places register and stack arguments in one sequence
    // of XLEN slots; every supported promoted type needs exactly one.
    const auto current = load(function, cursor, node);
    const auto stride = function.constant(8, difference_type());
    const auto next = function.emit(cursor.type, "ptr_add", binary_operands(current.id, stride));
    store(function, cursor, {cursor.type, next, std::nullopt}, node);
    return load(function, {type, current.id}, node);
  }

  const auto ap = lower_expr(function, args[0]);
  if (!ap.type.valid())
    return invalid_expr();
  const bool aarch64 = function.unit.source.native_aarch64_stdarg;
  if (name == "__cephyr_va_start") {
    function.emit_void(aarch64 ? "aarch64_va_start" : "amd64_va_start", id(ap.id));
    return {type, no_value, std::nullopt};
  }

  // SysV indexes from one save-area base; AAPCS64 uses negative offsets
  // from the end of each independent save area. Scalar slots always fit
  // exactly when an unused register remains. Both ABIs advance the shared
  // stack cursor by eight bytes. The va_list is evaluated exactly once.
  auto offset_type = integer_type(32, aarch64);
  auto pointer = pointer_type(void_type());
  offset_type.is_volatile = pointer.is_volatile = ap.type.element->is_volatile;
  const bool fp = type.real();
  const auto offset_field = (aarch64 ? 24 : 0) + (fp ? 4 : 0);
  const LValue offset_slot{offset_type, offset_address(function, ap.id, offset_field, offset_type)};
  const auto offset = load(function, offset_slot, node);
  const auto limit = function.constant(aarch64 ? 0 : (fp ? 176 : 48), offset_type);
  const auto in_register = function.emit(int_type(), aarch64 ? "slt" : "ult",
                                         binary_operands(offset.id, limit));
  const auto regs = function.block(), stack = function.block(), join = function.block();
  const auto address = function.param(join, pointer_type(type));
  function.branch(in_register, regs, stack);

  function.select(regs);
  const auto base_field = aarch64 && !fp ? 8 : 16;
  const auto base = load(function, {pointer, offset_address(function, ap.id, base_field, pointer)}, node);
  const auto reg_address = function.emit(pointer_type(type), "ptr_add", binary_operands(base.id, offset.id));
  const auto increment = function.constant(fp ? 16 : 8, offset_type);
  const auto next = function.emit(offset_type, "add", binary_operands(offset.id, increment));
  store(function, offset_slot, {offset_type, next, std::nullopt}, node);
  function.jump(join, {reg_address});

  function.select(stack);
  const LValue overflow_slot{pointer, offset_address(function, ap.id, aarch64 ? 0 : 8, pointer)};
  const auto overflow = load(function, overflow_slot, node);
  const auto stride = function.constant(8, difference_type());
  const auto next_overflow = function.emit(pointer, "ptr_add", binary_operands(overflow.id, stride));
  store(function, overflow_slot, {pointer, next_overflow, std::nullopt}, node);
  function.jump(join, {overflow.id});

  function.select(join);
  return load(function, {type, address}, node);
}

Expr direct_call(Function &function, cparser::CallExprASTNode *node,
                 const LValue &binding) {
  auto &symbol = *binding.linked;
  const auto &type = binding.type;
  if (!type.element) {
    function.error(node, "function declaration has no result type");
    return invalid_expr();
  }
  if (const auto error = record_abi_error(function.unit.source, type); !error.empty()) {
    function.error(node, error, "cephyr.abi.aggregate");
    return invalid_expr();
  }
  if (!type.prototype) {
    function.error(node, "calls through declarations without a prototype are "
                         "not supported");
    return invalid_expr();
  }
  const bool native_stdarg = function.unit.source.native_scalar_stdarg();
  if (type.variadic && !scalar_variadic_signature(type)) {
    function.error(node, "variadic calls do not support aggregate parameters or results");
    return invalid_expr();
  }
  if (type.variadic && !native_stdarg && (symbol.internal || symbol.definition)) {
    function.error(node, "variadic calls require an external declaration");
    return invalid_expr();
  }
  if (type.variadic && !native_stdarg && (type.element->real() ||
      std::any_of(type.params.begin(), type.params.end(), [](const CType &p) { return p.real(); }))) {
    function.error(node, "floating-point variadic calls are not supported");
    return invalid_expr();
  }

  const auto arguments = elements(node->getArgs());
  if (arguments.size() < type.params.size() ||
      (!type.variadic && arguments.size() != type.params.size())) {
    function.error(node, "function call has the wrong number of arguments");
    return invalid_expr();
  }
  if (arguments.size() > ARCHON_MIR_MAX_PARAMS) {
    function.error(node, "function call exceeds the native argument limit");
    return invalid_expr();
  }
  std::vector<Id> lowered;
  std::vector<CType> adjusted;
  lowered.reserve(arguments.size());
  if (type.variadic)
    adjusted.reserve(arguments.size());
  for (std::size_t index = 0; index != arguments.size(); ++index) {
    auto argument = lower_expr(function, arguments[index]);
    if (!argument.type.valid())
      return invalid_expr();
    if (index < type.params.size()) {
      argument =
          convert(function, argument, type.params[index], arguments[index]);
    } else if (argument.type.integer()) {
      argument = convert(function, argument, promote(argument.type),
                         arguments[index]);
    } else if (argument.type.real()) {
      if (argument.type.extended_real()) {
        function.error(arguments[index], "variadic long double arguments are not supported");
        return invalid_expr();
      }
      if (!native_stdarg) {
        function.error(arguments[index], "floating-point variadic calls require a bundled SysV AMD64, AAPCS64 or RV64IM LP64 backend");
        return invalid_expr();
      }
      argument = convert(function, argument, real_type(64), arguments[index]);
    } else if (!argument.type.pointer()) {
      function.error(arguments[index], native_stdarg
                         ? "variadic argument must be an integer, pointer or supported floating type"
                         : "variadic argument must be an integer or pointer");
      return invalid_expr();
    }
    if (!argument.type.valid())
      return invalid_expr();
    if (type.variadic)
      adjusted.push_back(argument.type);
    if (!append_abi_argument(function, argument, arguments[index], lowered))
      return invalid_expr();
  }

  std::string name_for_call = symbol.name;
  // A variadic declaration has no single native signature. Each promoted
  // call shape uses a fixed-signature import aliased to the actual symbol.
  if (type.variadic) {
    name_for_call = function.unit.require_variadic_call(symbol, adjusted);
    if (symbol.internal)
      symbol.used = true;
  } else
    symbol.used = true;
  const auto result_type = *type.element;
  if (result_type.kind == CType::Kind::record)
    return record_call_result(function, type, name_for_call, no_value, std::move(lowered), node);
  const auto operands = call_operands(name_for_call, lowered);
  if (is_void(result_type)) {
    function.emit_void("call", "- " + operands);
    return {result_type, no_value, std::nullopt};
  }
  Expr result{result_type,
              function.emit(result_type, "call", operands),
              std::nullopt};
  // The psABI representation of uint32_t is sign-extended. Re-establish the C
  // semantic value before it participates in an expression.
  return canonicalize(function, result, node);
}

Expr indirect_call(Function &function, cparser::CallExprASTNode *node) {
  auto callee = lower_expr(function, node->getExpr());
  if (!callee.type.valid())
    return invalid_expr();
  if (!callee.type.function_pointer()) {
    function.error(node, "called expression does not have function-pointer type");
    return invalid_expr();
  }
  const auto &type = *callee.type.element;
  if (!type.element) {
    function.error(node, "function-pointer type has no result type");
    return invalid_expr();
  }
  if (const auto error = record_abi_error(function.unit.source, type); !error.empty()) {
    function.error(node, error, "cephyr.abi.aggregate");
    return invalid_expr();
  }
  if (!type.prototype) {
    function.error(node, "indirect calls without a prototype are not supported");
    return invalid_expr();
  }
  if (type.variadic) {
    function.error(node, "variadic indirect calls are not supported");
    return invalid_expr();
  }

  const auto arguments = elements(node->getArgs());
  if (arguments.size() != type.params.size()) {
    function.error(node, "function call has the wrong number of arguments");
    return invalid_expr();
  }
  if (arguments.size() > ARCHON_MIR_MAX_PARAMS) {
    function.error(node, "function call exceeds the native argument limit");
    return invalid_expr();
  }
  std::vector<Id> lowered;
  lowered.reserve(arguments.size());
  for (std::size_t index = 0; index != arguments.size(); ++index) {
    auto argument = lower_expr(function, arguments[index]);
    argument =
        convert(function, argument, type.params[index], arguments[index]);
    if (!argument.type.valid())
      return invalid_expr();
    if (!append_abi_argument(function, argument, arguments[index], lowered))
      return invalid_expr();
  }

  const auto result_type = *type.element;
  if (result_type.kind == CType::Kind::record)
    return record_call_result(function, type, {}, callee.id, std::move(lowered), node);
  const auto operands = call_operands(id(callee.id) + ' ' + function.unit.require_signature(type), lowered);
  if (is_void(result_type)) {
    function.emit_void("call_indirect", "- " + operands);
    return {result_type, no_value, std::nullopt};
  }
  Expr result{result_type,
              function.emit(result_type, "call_indirect", operands),
              std::nullopt};
  return canonicalize(function, result, node);
}

Expr call(Function &function, cparser::CallExprASTNode *node) {
  auto *callee = node->getExpr();
  if (present(callee) && callee->getKind() == cparser::NK_GENERIC_EXPR) {
    callee = select_generic(callee, TypeContext(function.unit, &function));
    if (!callee)
      return invalid_expr();
  }
  if (present(callee) && callee->getKind() == cparser::NK_IDENT_NODE) {
    const auto name = identifier(callee);
    if (stdarg_builtin(name))
      return lower_stdarg_builtin(function, node, name);
    if (const auto binding = function.find_linked(name, callee)) {
      if (binding->type.kind == CType::Kind::function)
        return direct_call(function, node, *binding);
    } else if (!function.find_local(name)) {
      function.error(node, "called identifier is not a declared function");
      return invalid_expr();
    }
  }
  return indirect_call(function, node);
}

Expr string_literal(Function &function, Node *node) {
  const auto array = string_constant_type(node, function.unit.source);
  const auto symbol = function.unit.add_string(node, array);
  auto type = pointer_type(*array.element);
  return {type, function.emit(type, "global_addr", symbol), std::nullopt};
}

Expr lower_layout_constant(Function &function, Node *node) {
  const auto result = layout_constant(node, TypeContext(function.unit, &function));
  if (!result)
    return invalid_expr();
  return {result->type, function.constant(result->value, result->type), result->value};
}

Expr lower_binary(Function &function, Node *node) {
  const auto children = binary_children(node);
  if (!children) {
    function.error(node, "malformed binary expression");
    return invalid_expr();
  }
  if (node->getKind() == cparser::NK_LOG_AND_EXPR ||
      node->getKind() == cparser::NK_LOG_OR_EXPR)
    return logical(function, node->getKind(), children->lhs, children->rhs);

  if (node->getKind() == cparser::NK_MINUS_EXPR &&
      static_cast<cparser::MinusExprASTNode *>(node)->isUnary()) {
    auto value = lower_expr(function, children->rhs);
    if (value.type.extended_real()) {
      const auto word = integer_type(16, false);
      const auto sign = function.constant(0x8000, word);
      value.x87_sign_exponent = function.emit(
          word, "xor", binary_operands(value.x87_sign_exponent, sign), node);
      return value;
    }
    if (value.type.real())
      return {value.type, function.emit(value.type, "fneg", id(value.id)), std::nullopt};
    return arithmetic_binary(function, node->getKind(),
                             lower_expr(function, children->lhs), value, node);
  }
  auto lhs = lower_expr(function, children->lhs);
  auto rhs = lower_expr(function, children->rhs);
  switch (node->getKind()) {
  case cparser::NK_LT_EXPR:
  case cparser::NK_LE_EXPR:
  case cparser::NK_GT_EXPR:
  case cparser::NK_GE_EXPR:
  case cparser::NK_EQ_EXPR:
  case cparser::NK_NE_EXPR:
    return comparison(function, node->getKind(), lhs, rhs, node);
  default:
    return arithmetic_binary(function, node->getKind(), lhs, rhs, node);
  }
}

void attach_integer_constant(Function &function, Node *node, Expr &result) {
  if (!result.type.integer() || result.constant)
    return;
  Diagnostics probe;
  auto value = integer_constant(node, TypeContext(function.unit, &function).with_probe(probe));
  if (value)
    result.constant = normalize(value->value, result.type);
}

} // namespace

Expr load(Function &function, LValue value, Node *at) {
  if (!value.type.valid() || value.address == no_value)
    return invalid_expr();
  if (value.bit_field) {
    if (value.type.is_volatile)
      return read_volatile_bit_field(function, value, at);
    const auto field = *value.bit_field;
    const auto word = integer_type(64, false);
    Id storage = no_value;
    // Load exactly the bytes occupied by the field. The base-type unit may
    // extend outside the record member or be unaligned at this address.
    for (unsigned i = 0; i < field.bytes(); ++i) {
      const auto address = offset_address(function, value.address, i, integer_type(8, false));
      auto byte = function.emit(word, "load_u8", id(address) + " 0", at);
      if (i != 0)
        byte = function.emit(word, "shl", binary_operands(byte, function.constant(i * 8, word)));
      storage = i == 0 ? byte : function.emit(word, "or", binary_operands(storage, byte));
    }
    if (field.shift)
      storage = function.emit(word, "lshr", binary_operands(storage, function.constant(field.shift, word)));
    return narrow_bit_field(function, {word, storage, std::nullopt}, value);
  }
  if (value.type.kind == CType::Kind::array) {
    if (value.register_object) {
      function.error(at, "cannot convert a register array to a pointer");
      return invalid_expr();
    }
    return {pointer_type(array_element_type(value.type)), value.address,
            std::nullopt};
  }
  if (value.type.kind == CType::Kind::function)
    return {pointer_type(value.type), value.address, std::nullopt};
  if (value.type.kind == CType::Kind::record) {
    // Keep a value distinct from the source object, in particular for the
    // lifetime of array members of non-lvalue record expressions (6.2.4p8).
    auto temporary = function.local(unqualified(value.type), at);
    if (temporary.address == no_value ||
        !copy_record(function, temporary, {value.type, value.address, std::nullopt}, at, true))
      return invalid_expr();
    return {temporary.type, temporary.address, std::nullopt};
  }
  // Cephyr's MIR conservatively treats every memory operation as observable:
  // QML leaves it intact, and the native backends emit it in block order, even
  // if the result is unused. A scalar volatile access thus needs the same
  // width-specific opcode as an ordinary access. Any future load/store
  // optimization must distinguish volatile operations before rewriting them.
  if (value.type.pointer()) {
    auto result_type = unqualified(value.type);
    const auto operation =
        value.type.function_pointer() ? "load_func_ptr" : "load_ptr";
    return {result_type,
            function.emit(result_type, operation, id(value.address) + " 0", at),
            std::nullopt};
  }
  if (value.type.real()) {
    const auto type = unqualified(value.type);
    if (value.type.extended_real()) {
      // Snapshot the value in two integer SSA slots. Do not keep the source
      // address as the value: an assignment may overwrite it before reuse.
      // SysV AMD64 stores the 10-byte payload at offset zero; the remaining
      // six bytes are padding. Volatile accesses use these same ordered chunks.
      const auto low = function.emit(size_type(), "load", id(value.address) + " 0", at);
      const auto high = function.emit(integer_type(16, false), "load_u16",
                                      id(value.address) + " 8", at);
      return {type, low, std::nullopt, high};
    }
    return {type, function.emit(type, "fload", id(value.address) + " 0"), std::nullopt};
  }
  if (!value.type.integer()) {
    function.error(at, "loading this object type is not supported");
    return invalid_expr();
  }

  const char *operation = nullptr;
  switch (value.type.bits) {
  case 1:
  case 8:
    operation = value.type.is_signed ? "load_s8" : "load_u8";
    break;
  case 16:
    operation = value.type.is_signed ? "load_s16" : "load_u16";
    break;
  case 32:
    operation = value.type.is_signed ? "load_s32" : "load_u32";
    break;
  case 64:
    operation = "load";
    break;
  default:
    function.error(at, "unsupported integer object width");
    return invalid_expr();
  }
  auto result_type = unqualified(value.type);
  Expr result{result_type,
              function.emit(result_type, operation, id(value.address) + " 0", at),
              std::nullopt};
  return canonicalize(function, result, at);
}

void store(Function &function, LValue target, Expr value, Node *at) {
  if (!target.type.valid() || target.address == no_value ||
      !value.type.valid())
    return;
  if (target.type.is_const) {
    function.error(at, "assignment to a const-qualified object");
    return;
  }
  value = convert(function, value, target.type, at);
  if (!value.type.valid())
    return;
  if (target.bit_field) {
    if (target.type.is_volatile) {
      write_volatile_bit_field(function, target, value, at);
      return;
    }
    const auto field = *target.bit_field;
    const auto word = integer_type(64, false);
    auto positioned = function.emit(word, "and",
        binary_operands(value.id, function.constant(field.mask(), word)));
    if (field.shift)
      positioned = function.emit(word, "shl",
          binary_operands(positioned, function.constant(field.shift, word)));
    for (unsigned i = 0; i < field.bytes(); ++i) {
      const auto address = offset_address(function, target.address, i, integer_type(8, false));
      auto byte = positioned;
      if (i != 0)
        byte = function.emit(word, "lshr", binary_operands(byte, function.constant(i * 8, word)));
      const auto mask = ((field.mask() << field.shift) >> (i * 8)) & 0xff;
      if (mask != 0xff) {
        const auto previous = function.emit(word, "load_u8", id(address) + " 0", at);
        const auto kept = function.emit(word, "and",
            binary_operands(previous, function.constant(0xff ^ mask, word)));
        byte = function.emit(word, "or", binary_operands(byte, kept));
      }
      function.emit_void("store8", id(address) + " " + id(byte) + " 0");
    }
    return;
  }
  if (target.type.extended_real()) {
    function.emit_void("store", binary_operands(target.address, value.id) + " 0");
    function.emit_void("store16", binary_operands(target.address, value.x87_sign_exponent) + " 8");
    return;
  }

  const char *operation = nullptr;
  if (target.type.pointer())
    operation =
        target.type.function_pointer() ? "store_func_ptr" : "store_ptr";
  else if (target.type.real())
    operation = "fstore";
  else if (target.type.integer()) {
    switch (target.type.bits) {
    case 1:
    case 8:
      operation = "store8";
      break;
    case 16:
      operation = "store16";
      break;
    case 32:
      operation = "store32";
      break;
    case 64:
      operation = "store";
      break;
    default:
      break;
    }
  }
  if (!operation) {
    function.error(at, "storing this object type is not supported");
    return;
  }
  function.emit_void(operation,
                     id(target.address) + " " + id(value.id) + " 0");
}

Expr convert(Function &function, Expr value, const CType &target, Node *at,
             bool explicit_cast) {
  if (!value.type.valid() || !target.valid())
    return invalid_expr();
  if (is_void(target))
    return {target, no_value, std::nullopt};
  if (is_void(value.type)) {
    function.error(at, "void expression cannot be converted to a value");
    return invalid_expr();
  }
  if (!explicit_cast && target.kind == CType::Kind::record &&
      compatible(unqualified(target), value.type))
    return {unqualified(target), value.id, std::nullopt};

  if (target.integer() && target.bits == 1 && value.type.scalar()) {
    const auto boolean = truth(function, value, at);
    if (boolean == no_value)
      return invalid_expr();
    return {unqualified(target), boolean,
            value.type.integer() && value.constant
                ? std::optional<std::uint64_t>(*value.constant != 0)
                : std::nullopt};
  }

  if (target.extended_real() || value.type.extended_real()) {
    const auto type = unqualified(target);
    if (target.extended_real() && value.type.extended_real()) {
      value.type = type;
      return value;
    }
    if (!target.arithmetic() || !value.type.arithmetic()) {
      function.error(at, "long double conversions require arithmetic types");
      return invalid_expr();
    }
    if (target.extended_real()) {
      const auto temporary = function.local(type, at);
      if (temporary.address == no_value)
        return invalid_expr();
      const char *operation = "amd64_x87_from_fp";
      if (value.type.integer()) {
        value = canonicalize(function, value, at);
        if (!value.type.valid())
          return invalid_expr();
        operation = value.type.is_signed ? "amd64_x87_from_si" : "amd64_x87_from_ui";
      }
      function.emit_void(operation, binary_operands(temporary.address, value.id));
      return load(function, temporary, at);
    }
    const auto temporary = x87_temporary(function, value, at);
    if (temporary.address == no_value)
      return invalid_expr();
    const char *operation = target.real() ? "amd64_x87_to_fp" :
        target.is_signed ? "amd64_x87_to_si" : "amd64_x87_to_ui";
    return canonicalize(function, {type, function.emit(type, operation, id(temporary.address), at),
                                   std::nullopt}, at);
  }

  if (target.real() && value.type.arithmetic()) {
    const auto type = unqualified(target);
    if (value.type.real() && type.bits == value.type.bits)
      return {type, value.id, std::nullopt};
    if (value.type.real()) {
      if (function.unit.source.native_rv64_soft_fp)
        return rv64_soft_fp_call(function, type.bits == 64 ? "__extendsfdf2" : "__truncdfsf2",
                               type, value, at);
      return {type, function.emit(type, "fcast", id(value.id)), std::nullopt};
    }
    value = canonicalize(function, value, at);
    if (!value.type.valid())
      return invalid_expr();
    if (function.unit.source.native_rv64_soft_fp)
      return rv64_soft_fp_call(function, std::string(value.type.is_signed ? "__floatdi" : "__floatundi") +
                                       (type.bits == 32 ? "sf" : "df"),
                             type, value, at);
    return {type, function.emit(type, value.type.is_signed ? "sitofp" : "uitofp", id(value.id)),
            std::nullopt};
  }
  if (target.integer() && value.type.real()) {
    const auto type = unqualified(target);
    if (function.unit.source.native_rv64_soft_fp) {
      auto result = rv64_soft_fp_call(function, std::string(type.is_signed ? "__fix" : "__fixuns") +
                                               (value.type.bits == 32 ? "sfdi" : "dfdi"),
                                     integer_type(64, type.is_signed, 4), value, at);
      result.type = type;
      return canonicalize(function, result, at);
    }
    const auto operation = type.is_signed ? "fptosi" : "fptoui";
    return canonicalize(function, {type, function.emit(type, operation, id(value.id)), std::nullopt}, at);
  }

  if (target.integer() && value.type.integer()) {
    if (target.bits == 0 || target.bits > 64) {
      function.error(at, "unsupported integer conversion width");
      return invalid_expr();
    }
    const auto result_type = unqualified(target);
    Expr result{result_type, value.id, value.constant};
    const char *operation = target.is_signed ? "sext" : "zext";
    result.id = function.emit(result_type, operation,
                               id(value.id) + " " + std::to_string(target.bits));
    if (result.constant)
      result.constant = normalize(*result.constant, target);
    return result;
  }

  if (target.pointer() && value.type.integer()) {
    if (!explicit_cast && !is_null_pointer_constant(value)) {
      function.error(at, "only an integer constant expression equal to zero "
                         "converts implicitly to a pointer");
      return invalid_expr();
    }
    const auto result_type = unqualified(target);
    const auto cast = target.function_pointer() ? "int_to_func_ptr" : "int_to_ptr";
    return {result_type,
            function.emit(result_type, cast, id(value.id)),
            explicit_cast && is_unqualified_void_pointer(target) &&
                    is_null_pointer_constant(value)
                ? std::optional<std::uint64_t>(0)
                : std::nullopt};
  }

  if (target.integer() && value.type.pointer()) {
    if (!explicit_cast) {
      function.error(at, "pointer to integer conversion requires an explicit "
                         "cast");
      return invalid_expr();
    }
    const auto cast = value.type.function_pointer() ? "func_ptr_to_int" : "ptr_to_int";
    Expr result{unqualified(target),
                function.emit(size_type(), cast, id(value.id)),
                std::nullopt};
    return canonicalize(function, result, at);
  }

  if (target.pointer() && value.type.pointer()) {
    if (target.function_pointer() != value.type.function_pointer()) {
      // `(void *)0` is a C null pointer constant, even when the destination
      // is a function pointer. This materializes a null, not a data/code cast.
      if (target.function_pointer() && is_null_pointer_constant(value)) {
        const auto result_type = unqualified(target);
        const auto zero = function.constant(0, size_type());
        return {result_type,
                function.emit(result_type, "int_to_func_ptr", id(zero)),
                std::nullopt};
      }
      function.error(at, "conversion between function and object pointers is "
                         "not supported");
      return invalid_expr();
    }
    if (target.function_pointer()) {
      if (!explicit_cast && !compatible(unqualified(target), value.type)) {
        function.error(at, "incompatible function-pointer conversion");
        return invalid_expr();
      }
      return {unqualified(target), value.id, std::nullopt};
    }
    if (!explicit_cast &&
        (!compatible_object_pointers(target, value.type) ||
         drops_qualifiers(value.type, target))) {
      function.error(at, "incompatible object-pointer conversion");
      return invalid_expr();
    }
    return {unqualified(target), value.id, std::nullopt};
  }

  function.error(at, "unsupported scalar conversion");
  return invalid_expr();
}

Id truth(Function &function, Expr value, Node *at) {
  if (!value.type.valid())
    return no_value;
  if (value.type.extended_real()) {
    const auto zero = x87_constant(function, Constant{value.type, 0});
    const auto is_zero = x87_compare(function, "amd64_x87_eq", value, zero, at);
    return is_zero == no_value ? no_value : invert(function, is_zero);
  }
  if (value.type.real()) {
    const auto zero = floating_bits(function, value.type, 0);
    const auto is_zero = function.emit(int_type(), "feq", binary_operands(value.id, zero));
    return invert(function, is_zero);
  }
  Id bits = value.id;
  if (value.type.pointer()) {
    const auto cast = value.type.function_pointer() ? "func_ptr_to_int" : "ptr_to_int";
    bits = function.emit(size_type(), cast, id(value.id));
  } else if (!value.type.integer()) {
    function.error(at, "condition requires a scalar expression");
    return no_value;
  }
  const auto zero = function.constant(0, size_type());
  const auto is_zero =
      function.emit(int_type(), "eq", binary_operands(bits, zero));
  return invert(function, is_zero);
}

Id abi_value(Function &function, Expr value) {
  if (!value.type.integer() || value.id == no_value)
    return value.id;
  if (value.type.bits > 32)
    return value.id;
  Id semantic = value.id;
  if (!value.type.is_signed)
    semantic = function.emit(value.type, "zext",
                             id(semantic) + " " +
                                 std::to_string(value.type.bits));
  // RV64 psABI first widens narrow scalars to 32 bits according to C signedness
  // and then sign-extends that 32-bit slot to XLEN. This is intentionally also
  // done for uint32_t. SysV AMD64 and little-endian AAPCS64 permit this
  // extension: the callee needs only the declared low bits of the scalar.
  return function.emit(integer_type(64, true), "sext",
                       id(semantic) + " 32");
}

static LValue lower_member(Function &function, LValue object, Node *name) {
  const auto member = find_member(object.type, identifier(name));
  if (!member) {
    member_type(object.type, name, TypeContext(function.unit, &function));
    return invalid_lvalue();
  }
  if (object.address == no_value)
    return invalid_lvalue();
  auto offset = member->offset;
  if (member->type.is_volatile && member->bit_field &&
      member->bit_field->volatile_access != BitField::VolatileAccess::unsupported)
    offset -= member->bit_field->byte_in_container;
  return {member->type, offset_address(function, object.address, offset, member->type), {},
           object.register_object, nullptr, member->bit_field};
}

LValue lower_lvalue(Function &function, Node *node) {
  const AnalysisDepth depth(function.expression_depth);
  if (!present(node)) {
    function.error(node, "expected an lvalue");
    return invalid_lvalue();
  }
  switch (node->getKind()) {
  case cparser::NK_GENERIC_EXPR: {
    auto *selected = select_generic(node, TypeContext(function.unit, &function));
    return selected ? lower_lvalue(function, selected) : invalid_lvalue();
  }
  case cparser::NK_IDENT_NODE: {
    const auto name = identifier(node);
    if (auto binding = function.find_linked(name, node)) {
      if (binding->type.kind == CType::Kind::function) {
        function.error(node, "function designator is not an object lvalue");
        return invalid_lvalue();
      }
      binding->linked->used = true;
      binding->address = function.emit(pointer_type(binding->type), "global_addr",
                                       binding->linked->name);
      return *binding;
    }
    if (auto local = function.find_local(name)) {
      if (!local->static_name.empty()) {
        function.require_static(*local);
        local->address = function.emit(pointer_type(local->type), "global_addr",
                                       local->static_name);
      }
      return *local;
    }
    function.error(node, "use of undeclared identifier");
    return invalid_lvalue();
  }
  case cparser::NK_STRUCT_REF: {
    auto *dot = static_cast<cparser::StructRefASTNode *>(node);
    return lower_member(function, lower_lvalue(function, dot->getName()), dot->getMember());
  }
  case cparser::NK_INDIRECT_REF: {
    auto *indirect = static_cast<cparser::IndirectRefASTNode *>(node);
    auto pointer = lower_expr(function, indirect->getExpr());
    if (!pointer.type.valid())
      return invalid_lvalue();
    if (!pointer.type.pointer() || !pointer.type.element) {
      function.error(node, present(indirect->getField()) ?
                     "'->' requires a pointer to a structure or union" :
                     "indirection requires a pointer operand");
      return invalid_lvalue();
    }
    if (present(indirect->getField()))
      return lower_member(function, {*pointer.type.element, pointer.id}, indirect->getField());
    if (pointer.type.function_pointer()) {
      function.error(node, "function designator is not an object lvalue");
      return invalid_lvalue();
    }
    return {*pointer.type.element, pointer.id};
  }
  case cparser::NK_ARRAY_REF: {
    auto *array = static_cast<cparser::ArrayRefASTNode *>(node);
    auto base = lower_expr(function, array->getExpr());
    auto index = lower_expr(function, array->getIndex());
    if (!base.type.valid() || !index.type.valid())
      return invalid_lvalue();
    if (base.type.integer() && index.type.pointer())
      std::swap(base, index);
    if (!base.type.pointer() || !base.type.element || !index.type.integer()) {
      function.error(node, "subscript requires a pointer and an integer");
      return invalid_lvalue();
    }
    auto address = pointer_offset(function, base, index, false, node);
    return address.type.valid()
               ? LValue{*address.type.element, address.id}
               : invalid_lvalue();
  }
  case cparser::NK_STRING_CONST: {
    auto array = string_constant_type(node, function.unit.source);
    const auto symbol = function.unit.add_string(node, array);
    return {array, function.emit(pointer_type(array), "global_addr", symbol)};
  }
  case cparser::NK_COMPOUND_LITERAL:
    return lower_compound_literal(function, node);
  default:
    function.error(node, "expression is not an lvalue");
    return invalid_lvalue();
  }
}

Expr lower_expr(Function &function, Node *node) {
  const AnalysisDepth depth(function.expression_depth);
  if (!present(node)) {
    function.error(node, "missing expression");
    return invalid_expr();
  }

  Expr result;
  switch (node->getKind()) {
  case cparser::NK_GENERIC_EXPR: {
    auto *selected = select_generic(node, TypeContext(function.unit, &function));
    return selected ? lower_expr(function, selected) : invalid_expr();
  }
  case cparser::NK_INTEGER_CONST:
  case cparser::NK_CHAR_CONST: {
    auto constant = integer_constant(node, TypeContext(function.unit, &function));
    if (!constant)
      return invalid_expr();
    constant->value = normalize(constant->value, constant->type);
    result = {constant->type,
              function.constant(constant->value, constant->type),
              constant->value};
    break;
  }
  case cparser::NK_STRING_CONST:
    result = string_literal(function, node);
    break;
  case cparser::NK_COMPOUND_LITERAL:
    return load(function, lower_compound_literal(function, node), node);
  case cparser::NK_REAL_CONST: {
    const auto value = real_constant(node, TypeContext(function.unit, &function));
    if (!value)
      return invalid_expr();
    result = value->type.extended_real()
        ? x87_constant(function, *value)
        : Expr{value->type, floating_bits(function, value->type, value->value), std::nullopt};
    break;
  }
  case cparser::NK_IDENT_NODE: {
    const auto name = identifier(node);
    if (const auto binding = function.find_linked(name, node);
        binding && binding->type.kind == CType::Kind::function) {
      binding->linked->used = true;
      const auto type = pointer_type(binding->type);
      result = {type, function.emit(type, "func_addr", binding->linked->name), std::nullopt};
      break;
    }
    result = load(function, lower_lvalue(function, node), node);
    break;
  }
  case cparser::NK_CAST_EXPR: {
    auto *cast = static_cast<cparser::CastExprASTNode *>(node);
    const auto context = TypeContext(function.unit, &function);
    const auto type = read_type(cast->getType(), context, true);
    if (type.integer() && present(cast->getExpr()) &&
        cast->getExpr()->getKind() == cparser::NK_REAL_CONST) {
      // C11 6.6p6 permits this as an ICE. Use the semantic conversion
      // directly so even a runtime expression needs no FP MachineIR.
      const auto errors = function.unit.diagnostics.error_count();
      const auto value = integer_constant(node, context);
      if (!value) {
        if (function.unit.diagnostics.error_count() == errors)
          function.error(node, "unsupported floating constant in integer cast");
        return invalid_expr();
      }
      result = {value->type, function.constant(value->value, value->type), value->value};
      break;
    }
    result = convert(function, lower_expr(function, cast->getExpr()), type, node,
                     true);
    break;
  }
  case cparser::NK_UNARY_PLUS_EXPR: {
    auto *plus = static_cast<cparser::UnaryPlusASTNode *>(node);
    auto operand = lower_expr(function, plus->getExpr());
    if (operand.type.real()) {
      result = operand;
      break;
    }
    if (!operand.type.integer()) {
      function.error(node, "unary plus requires an arithmetic operand");
      return invalid_expr();
    }
    result = convert(function, operand, promote(operand.type), node);
    break;
  }
  case cparser::NK_BIT_NOT_EXPR: {
    auto *bit_not = static_cast<cparser::BitNotExprASTNode *>(node);
    auto operand = lower_expr(function, bit_not->getExpr());
    if (!operand.type.integer()) {
      function.error(node, "bitwise complement requires an integer operand");
      return invalid_expr();
    }
    const auto type = promote(operand.type);
    operand = convert(function, operand, type, node);
    const auto all_ones = function.constant(~std::uint64_t{0}, type);
    result = {type,
              function.emit(type, "xor",
                            binary_operands(operand.id, all_ones)),
              std::nullopt};
    result = canonicalize(function, result, node);
    break;
  }
  case cparser::NK_LOG_NOT_EXPR: {
    auto *logical_not = static_cast<cparser::LogNotExprASTNode *>(node);
    const auto condition =
        truth(function, lower_expr(function, logical_not->getExpr()), node);
    if (condition == no_value)
      return invalid_expr();
    result = {int_type(), invert(function, condition), std::nullopt};
    break;
  }
  case cparser::NK_PREDECREMENT_EXPR:
    result = increment(
        function,
        static_cast<cparser::PredecrementExprASTNode *>(node)->getExpr(), true,
        false, node);
    break;
  case cparser::NK_PREINCREMENT_EXPR:
    result = increment(
        function,
        static_cast<cparser::PreincrementExprASTNode *>(node)->getExpr(), false,
        false, node);
    break;
  case cparser::NK_POSTDECREMENT_EXPR:
    result = increment(
        function,
        static_cast<cparser::PostdecrementExprASTNode *>(node)->getExpr(), true,
        true, node);
    break;
  case cparser::NK_POSTINCREMENT_EXPR:
    result = increment(
        function,
        static_cast<cparser::PostincrementExprASTNode *>(node)->getExpr(), false,
        true, node);
    break;
  case cparser::NK_ADDR_EXPR: {
    auto *address = static_cast<cparser::AddrExprASTNode *>(node);
    auto *operand = address->getExpr();
    if (!present(operand)) {
      function.error(node, "address operator requires an operand");
      return invalid_expr();
    }
    if (operand->getKind() == cparser::NK_GENERIC_EXPR) {
      operand = select_generic(operand, TypeContext(function.unit, &function));
      if (!operand)
        return invalid_expr();
    }
    if (operand->getKind() == cparser::NK_INDIRECT_REF) {
      auto *indirect = static_cast<cparser::IndirectRefASTNode *>(operand);
      if (!present(indirect->getField())) {
        auto pointer = lower_expr(function, indirect->getExpr());
        if (!pointer.type.pointer()) {
          if (pointer.type.valid())
            function.error(node, "indirection requires a pointer operand");
          return invalid_expr();
        }
        // C11 6.5.3.2: neither & nor * is evaluated, including for null.
        pointer.constant.reset();
        result = std::move(pointer);
        break;
      }
    }
    if (operand->getKind() == cparser::NK_IDENT_NODE) {
      const auto name = identifier(operand);
      if (const auto binding = function.find_linked(name, operand);
          binding && binding->type.kind == CType::Kind::function) {
        binding->linked->used = true;
        const auto type = pointer_type(binding->type);
        result = {type, function.emit(type, "func_addr", binding->linked->name), std::nullopt};
        break;
      }
    }
    auto lvalue = lower_lvalue(function, operand);
    if (lvalue.bit_field) {
      function.error(operand, "cannot take the address of a bit-field", "cephyr.type");
      return invalid_expr();
    }
    if (lvalue.register_object) {
      function.error(operand, "cannot take the address of a register object", "cephyr.type");
      return invalid_expr();
    }
    result = lvalue.type.valid()
                 ? Expr{pointer_type(lvalue.type), lvalue.address, std::nullopt}
                 : invalid_expr();
    break;
  }
  case cparser::NK_INDIRECT_REF: {
    auto *indirect = static_cast<cparser::IndirectRefASTNode *>(node);
    if (present(indirect->getField())) {
      result = load(function, lower_lvalue(function, node), node);
      break;
    }
    auto pointer = lower_expr(function, indirect->getExpr());
    if (!pointer.type.pointer() || !pointer.type.element) {
      function.error(node, "indirection requires a pointer operand");
      return invalid_expr();
    }
    if (pointer.type.function_pointer()) {
      result = pointer;
      break;
    }
    result = load(function, {*pointer.type.element, pointer.id}, node);
    break;
  }
  case cparser::NK_ARRAY_REF:
    result = load(function, lower_lvalue(function, node), node);
    break;
  case cparser::NK_LSHIFT_EXPR:
  case cparser::NK_RSHIFT_EXPR:
  case cparser::NK_BIT_IOR_EXPR:
  case cparser::NK_BIT_XOR_EXPR:
  case cparser::NK_BIT_AND_EXPR:
  case cparser::NK_LOG_AND_EXPR:
  case cparser::NK_LOG_OR_EXPR:
  case cparser::NK_PLUS_EXPR:
  case cparser::NK_MINUS_EXPR:
  case cparser::NK_MULT_EXPR:
  case cparser::NK_TRUNC_DIV_EXPR:
  case cparser::NK_TRUNC_MOD_EXPR:
  case cparser::NK_LT_EXPR:
  case cparser::NK_LE_EXPR:
  case cparser::NK_GT_EXPR:
  case cparser::NK_GE_EXPR:
  case cparser::NK_EQ_EXPR:
  case cparser::NK_NE_EXPR:
    result = lower_binary(function, node);
    break;
  case cparser::NK_ASSIGN_EXPR:
    result =
        assignment(function, static_cast<cparser::AssignExprASTNode *>(node));
    break;
  case cparser::NK_COND_EXPR:
    result =
        conditional(function, static_cast<cparser::CondExprASTNode *>(node));
    break;
  case cparser::NK_CALL_EXPR:
    result = call(function, static_cast<cparser::CallExprASTNode *>(node));
    break;
  case cparser::NK_SIZEOF_EXPR:
  case cparser::NK_ALIGNOF_EXPR:
  case cparser::NK_OFFSETOF_EXPR:
    result = lower_layout_constant(function, node);
    break;
  case cparser::NK_LIST: {
    const auto expressions = elements(node);
    if (expressions.empty()) {
      result = {void_type(), no_value, std::nullopt};
      break;
    }
    for (auto *expression : expressions)
      result = lower_expr(function, expression);
    result.constant.reset();
    result.type.denotes_bit_field = false;
    break;
  }
  case cparser::NK_COMPOUND_EXPR: {
    auto *compound = static_cast<cparser::CompoundExprASTNode *>(node);
    const auto expressions = elements(compound->getElements());
    if (expressions.empty()) {
      function.error(node, "comma expression has no operands");
      return invalid_expr();
    }
    for (auto *expression : expressions)
      result = lower_expr(function, expression);
    result.constant.reset();
    result.type.denotes_bit_field = false;
    break;
  }
  case cparser::NK_NOP_EXPR:
    result = {void_type(), no_value, std::nullopt};
    break;
  case cparser::NK_COMPLEX_CONST:
  case cparser::NK_FLOAT_EXPR:
  case cparser::NK_COMPLEX_EXPR:
  case cparser::NK_FIX_TRUNC_EXPR:
  case cparser::NK_RDIV_EXPR:
    function.error(node, "floating-point expressions are not supported");
    return invalid_expr();
  case cparser::NK_STRUCT_REF: {
    auto *dot = static_cast<cparser::StructRefASTNode *>(node);
    if (describes_lvalue(dot->getName(), TypeContext(function.unit, &function))) {
      result = load(function, lower_lvalue(function, node), node);
    } else {
      const auto record = lower_expr(function, dot->getName());
      result = load(function, lower_member(function, {record.type, record.id}, dot->getMember()), node);
    }
    break;
  }
  case cparser::NK_NEGATE_EXPR:
  case cparser::NK_NON_LVALUE_EXPR:
    function.error(node, "expression node is not supported by the parser "
                         "adapter");
    return invalid_expr();
  default:
    function.error(node, "unsupported expression node");
    return invalid_expr();
  }
  attach_integer_constant(function, node, result);
  return result;
}

} // namespace cephyr::detail
