#include "novostone/ActionSemantics/ActionSemantics.hpp"
#include "Model.hpp"
#include "Syntax.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <mutex>
#include <set>

namespace novostone::action {
namespace {
using namespace detail;
std::pair<std::size_t, std::size_t> location(std::string_view text, std::size_t at) {
  std::size_t line = 1, column = 1;
  for (std::size_t i = 0; i < std::min(at, text.size()); ++i)
    if (text[i] == '\n') { ++line; column = 1; } else ++column;
  return {line, column};
}
std::string message(const std::string& source, std::string_view text, std::size_t at, const std::string& reason) {
  auto [line, column] = location(text, at);
  return source + ':' + std::to_string(line) + ':' + std::to_string(column) + ": " + reason;
}
std::string str(novo_as_string_t s) { return {s.data, s.length}; }
void require(bool condition, const std::string& reason) {
  if (!condition) throw std::invalid_argument(reason);
}
void name(const std::string& s) { require(s.size() <= 64, "identifier exceeds 64 bytes"); }

struct Lower {
  Model& model;
  const Constructor& ctor;
  std::size_t& offset;
  bool extended;                 // set once any dialect-2-only construct is lowered
  std::map<novo_as_expr_t, int> expressions;
  std::map<novo_as_action_t, int> actions;
  std::map<novo_as_expr_t, int> types; // 0 integer, 1 token, 2 cell, 3 dynamic
  int field(const std::string& n) const {
    for (std::size_t i = 0; i < ctor.fields.size(); ++i)
      if (ctor.fields[i].name == n) return static_cast<int>(i);
    throw std::invalid_argument("unknown field '" + n + "' in " + ctor.name);
  }
  void expect(novo_as_expr_t e, int type) {
    require(types.at(e) == type || types.at(e) == 3, "incompatible yielder type");
  }
  int intern_token(const std::string& spelling) {
    std::string s = spelling;
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    name(s);
    auto it = std::find(model.tokens.begin(), model.tokens.end(), s);
    int index = static_cast<int>(it - model.tokens.begin());
    if (it == model.tokens.end()) model.tokens.push_back(s);
    require(model.tokens.size() <= 256, "token limit exceeded");
    return index;
  }
  void operands(const novo_as_expr_t* items, std::size_t length) {
    for (std::size_t j = 0; j < length; ++j) model.pool.push_back(0);
    for (std::size_t j = 0; j < length; ++j)
      model.pool[model.pool.size() - length + j] = expressions.at(items[j]);
  }
  void budget() {
    require(model.expressions.size() + model.actions.size() < 8192, "instruction limit exceeded");
  }
};
// Leave-callbacks instantiated by the generated postorder visitor; operands are
// lowered before their uses, so referenced pool indices already exist.
int leave_expr(Lower* c, novo_as_expr_t e) {
  c->offset = e->offset;
  Instruction i;
  int type = 0;
  switch (e->kind) {
    case novo_as_Integer_kind: {
      auto s = str(e->v.Integer.spelling);
      auto p = std::from_chars(s.data(), s.data() + s.size(), i.value);
      require(p.ec == std::errc{} && p.ptr == s.data() + s.size() &&
        i.value >= -2147483648LL && i.value <= 2147483647LL, "integer outside signed 32-bit range");
      i.op = 0; break;
    }
    case novo_as_Field_kind:
      i.op = 1; i.a = c->field(str(e->v.Field.name));
      type = c->ctor.fields[static_cast<std::size_t>(i.a)].type;
      require(type < 2, "syntax fields must be used with evaluate(field)"); break;
    case novo_as_Token_kind:
      i.op = 2; type = 1; i.a = c->intern_token(str(e->v.Token.name)); break;
    case novo_as_Unary_kind: {
      auto& u = e->v.Unary; auto op = str(u.op); i.a = c->expressions.at(u.operand);
      if (op == "given") {
        const auto& n = c->model.expressions[static_cast<std::size_t>(i.a)];
        require(n.op == 0 && n.value >= 1 && n.value <= 64, "given index must be in 1..64");
        i.op = 3; i.a = static_cast<int>(n.value - 1); type = 3;
      } else if (op == "bound") { c->expect(u.operand, 1); i.op = 4; type = 3; }
      else { c->expect(u.operand, 2); i.op = 5; type = c->model.tagged_cells ? 3 : 0; }
      break;
    }
    case novo_as_Binary_kind: {
      auto& b = e->v.Binary; c->expect(b.left, 0); c->expect(b.right, 0);
      static const std::map<std::string, int> ops{{"+", 6}, {"-", 7}, {"*", 8}, {"/", 9}, {"==", 10}, {"<", 11}};
      i.op = ops.at(str(b.op)); i.a = c->expressions.at(b.left); i.b = c->expressions.at(b.right); break;
    }
  }
  c->budget();
  c->expressions[e] = static_cast<int>(c->model.expressions.size()); c->types[e] = type;
  c->model.expressions.push_back(i); return 0;
}
int leave_action(Lower* c, novo_as_action_t a) {
  c->offset = a->offset;
  Instruction i;
  switch (a->kind) {
    case novo_as_Conditional_kind:
      c->extended = true;
      c->expect(a->v.Conditional.condition, 0);
      i.op = 103; i.a = c->expressions.at(a->v.Conditional.condition);
      i.b = c->actions.at(a->v.Conditional.yes); i.value = c->actions.at(a->v.Conditional.no); break;
    case novo_as_While_kind:
      c->extended = true;
      c->expect(a->v.While.condition, 0);
      i.op = 104; i.a = c->expressions.at(a->v.While.condition);
      i.b = c->actions.at(a->v.While.body); break;
    case novo_as_Break_kind: case novo_as_Continue_kind:
      c->extended = true;
      i.op = a->kind == novo_as_Break_kind ? 105 : 106; break;
    case novo_as_Generator_kind:
      c->extended = true;
      i.op = CreateGenerator; i.a = c->actions.at(a->v.Generator.body); break;
    case novo_as_YieldValue_kind:
      c->extended = true;
      i.op = YieldValue; i.a = c->expressions.at(a->v.YieldValue.value); break;
    case novo_as_Next_kind:
      c->extended = true;
      i.op = Next; i.a = c->expressions.at(a->v.Next.iterator); break;
    case novo_as_Close_kind:
      c->extended = true;
      i.op = Close; i.a = c->expressions.at(a->v.Close.iterator); break;
    case novo_as_CallCC_kind:
      c->extended = true;
      i.op = CallCC; i.a = c->expressions.at(a->v.CallCC.function); break;
    case novo_as_Resume_kind:
      c->extended = true;
      i.op = ResumeCont; i.a = c->expressions.at(a->v.Resume.continuation);
      i.b = c->expressions.at(a->v.Resume.value); break;
    case novo_as_Unfolding_kind:
      c->extended = true;
      i.op = Unfolding; i.a = c->actions.at(a->v.Unfolding.body); break;
    case novo_as_Unfold_kind: case novo_as_TailUnfold_kind:
      c->extended = true;
      i.op = a->kind == novo_as_Unfold_kind ? Unfold : TailUnfold; break;
    case novo_as_Closure_kind:
      c->extended = true;
      i.op = 16; i.a = c->actions.at(a->v.Closure.body); break;
    case novo_as_Enact_kind:
      c->extended = true;
      i.op = 20; i.a = c->expressions.at(a->v.Enact.function); break;
    case novo_as_Apply_kind: case novo_as_TailCall_kind: {
      novo_as_expr_t function;
      const novo_as_expr_t* items;
      std::size_t length;
      if (a->kind == novo_as_Apply_kind) {
        function = a->v.Apply.function;
        length = a->v.Apply.arguments.length;
        items = a->v.Apply.arguments.items;
      } else {
        function = a->v.TailCall.function;
        length = a->v.TailCall.arguments.length;
        items = a->v.TailCall.arguments.items;
      }
      c->extended = true;
      i.op = a->kind == novo_as_Apply_kind ? 17 : 19;
      require(length <= 64, "application exceeds 64 arguments");
      i.a = c->expressions.at(function);
      i.pool = static_cast<int>(c->model.pool.size());
      i.count = static_cast<int>(length);
      c->operands(items, length);
      break;
    }
    case novo_as_LetRec_kind: {
      auto& r = a->v.LetRec;
      std::set<std::string> names;
      i.op = 21;
      i.b = c->actions.at(r.body);
      i.pool = static_cast<int>(c->model.pool.size());
      i.count = static_cast<int>(r.bindings.length);
      require(r.bindings.length >= 1, "letrec requires at least one binding");
      for (std::size_t j = 0; j < r.bindings.length; ++j) {
        auto binding = r.bindings.items[j];
        require(binding->kind == novo_as_Binding_kind, "invalid letrec binding");
        require(binding->v.Binding.body->kind == novo_as_Closure_kind, "letrec bindings must be closures");
        require(names.insert(str(binding->v.Binding.name)).second,
          "duplicate letrec binding: " + str(binding->v.Binding.name));
        c->model.pool.push_back(c->intern_token(str(binding->v.Binding.name)));
        c->model.pool.push_back(c->actions.at(binding->v.Binding.body->v.Closure.body));
      }
      c->extended = true;
      break;
    }
    case novo_as_Return_kind:
      c->extended = true;
      i.op = 18; i.a = c->expressions.at(a->v.Return.value); break;
    default: break;
  }
  if (i.op < 0) {
    if (a->kind == novo_as_Evaluate_kind) {
      i.op = 8; i.a = c->field(str(a->v.Evaluate.field));
      require(c->ctor.fields[static_cast<std::size_t>(i.a)].type >= 2, "evaluate requires a syntax field");
    } else if (a->kind == novo_as_Furthermore_kind) {
      i.op = 14; i.a = c->actions.at(a->v.Furthermore.body);
    } else if (a->kind == novo_as_Combine_kind) {
      auto& b = a->v.Combine;
      static const std::map<std::string, int> ops{{"then", 9}, {"and then", 10}, {"hence", 11},
        {"otherwise", 12}, {"trapping", 13}};
      i.op = ops.at(str(b.op)); i.a = c->actions.at(b.left); i.b = c->actions.at(b.right);
    } else {
      require(a->kind == novo_as_Primitive_kind, "unsupported action");
      auto& p = a->v.Primitive;
      static const std::map<std::string, int> ops{{"complete", 0}, {"fail", 1}, {"give", 2},
        {"check", 3}, {"escape", 4}, {"bind", 5}, {"store", 6}, {"allocate", 7}, {"put", 102}};
      i.op = ops.at(str(p.op));
      if (p.operands.length) i.a = c->expressions.at(p.operands.items[0]);
      if (p.operands.length > 1) i.b = c->expressions.at(p.operands.items[1]);
      if (i.op == 3 || i.op == 6) c->expect(p.operands.items[0], 0);
      if (i.op == 5) c->expect(p.operands.items[0], 1);
      if (i.op == 6 || i.op == 102) c->expect(p.operands.items[1], 2);
      if (i.op == 102) c->extended = true;
    }
  }
  c->budget();
  c->actions[a] = static_cast<int>(c->model.actions.size());
  c->model.actions.push_back(i); return 0;
}
#define NOVO_AS_VISITOR_NAME lower
#define NOVO_AS_VISITOR_CONTEXT Lower
#define NOVO_AS_LEAVE_expr leave_expr
#define NOVO_AS_LEAVE_action leave_action
#define NOVO_AS_VISITOR_MAX_DEPTH 128
#include "ActionSemanticsVisitor.h"
#undef NOVO_AS_VISITOR_NAME
#undef NOVO_AS_VISITOR_CONTEXT
#undef NOVO_AS_LEAVE_expr
#undef NOVO_AS_LEAVE_action
#undef NOVO_AS_VISITOR_MAX_DEPTH
// Detect tagged stores before type checking any rule: a later rule may create
// cells read by an earlier rule. Integer-only legacy specifications retain
// their stronger contents() type validation.
int detect_tagged_cells(bool* tagged, novo_as_action_t a) {
  if (a->kind == novo_as_Primitive_kind && str(a->v.Primitive.op) == "put") *tagged = true;
  return 0;
}
#define NOVO_AS_VISITOR_NAME detect_cells
#define NOVO_AS_VISITOR_CONTEXT bool
#define NOVO_AS_LEAVE_action detect_tagged_cells
#define NOVO_AS_VISITOR_MAX_DEPTH 128
#include "ActionSemanticsVisitor.h"
#undef NOVO_AS_VISITOR_NAME
#undef NOVO_AS_VISITOR_CONTEXT
#undef NOVO_AS_LEAVE_action
#undef NOVO_AS_VISITOR_MAX_DEPTH

std::shared_ptr<Model> build_model(novo_as_document_t d, const std::string& source,
                                 std::size_t& offset) {
  auto model = std::make_shared<Model>();
  model->name = str(d->name); model->source = source;
  name(model->name);
  std::map<std::string, int> sorts, seen;
  for (std::size_t j = 0; j < d->sorts.length; ++j) {
    auto s = d->sorts.items[j]; offset = s->offset; auto n = str(s->name); name(n);
    require(n != "integer" && n != "identifier", "reserved sort: " + n);
    require(sorts.emplace(n, static_cast<int>(j)).second, "duplicate sort: " + n);
    model->sorts.push_back(n);
  }
  require(sorts.count(str(d->start)), "unknown start sort: " + str(d->start));
  model->start = sorts.at(str(d->start));
  for (std::size_t j = 0; j < d->sorts.length; ++j) {
    auto s = d->sorts.items[j];
    for (std::size_t k = 0; k < s->constructors.length; ++k) {
      auto c = s->constructors.items[k];
      offset = c->offset;
      Constructor ctor{str(c->name), static_cast<int>(j), -1, {}};
      name(ctor.name);
      require(seen.emplace(ctor.name, static_cast<int>(model->constructors.size())).second,
        "duplicate constructor: " + ctor.name);
      require(c->fields.length <= 16, "constructor exceeds 16 fields");
      std::set<std::string> fields;
      for (std::size_t f = 0; f < c->fields.length; ++f) {
        auto field = c->fields.items[f];
        offset = field->offset;
        auto n = str(field->name), t = str(field->type); name(n);
        require(fields.insert(n).second, "duplicate field: " + n);
        require(t == "integer" || t == "identifier" || sorts.count(t), "unknown field sort: " + t);
        ctor.fields.push_back({n, t == "integer" ? 0 : t == "identifier" ? 1 : 2 + sorts.at(t)});
      }
      model->constructors.push_back(std::move(ctor));
      require(model->constructors.size() <= 256, "constructor limit exceeded");
    }
  }
  for (std::size_t j = 0; j < d->rules.length; ++j) {
    auto r = d->rules.items[j]; offset = r->offset;
    require(detect_cells_action(r->body, &model->tagged_cells) == 0,
            "action exceeds visitor depth limit (128)");
  }
  for (std::size_t j = 0; j < d->rules.length; ++j) {
    auto r = d->rules.items[j];
    offset = r->offset;
    auto n = str(r->name);
    require(seen.count(n), "rule for unknown constructor: " + n);
    auto& ctor = model->constructors[static_cast<std::size_t>(seen.at(n))];
    require(ctor.rule == -1, "duplicate rule: " + n);
    Lower lowerer{*model, ctor, offset, false, {}, {}, {}};
    require(lower_action(r->body, &lowerer) == 0, "action exceeds visitor depth limit (128)");
    ctor.rule = lowerer.actions.at(r->body);
    model->requires_extended |= lowerer.extended;
  }
  require(model->requires_extended || model->expressions.size() + model->actions.size() <= 4096,
          "instruction limit exceeded");
  offset = 0;
  for (const auto& c : model->constructors) require(c.rule >= 0, "missing rule: " + c.name);
  std::set<int> productive;
  bool changed;
  do {
    changed = false;
    for (const auto& c : model->constructors) {
      bool ready = std::all_of(c.fields.begin(), c.fields.end(),
        [&](const auto& f) { return f.type < 2 || productive.count(f.type - 2); });
      if (ready) changed |= productive.insert(c.sort).second;
    }
  } while (changed);
  require(productive.size() == model->sorts.size(), "syntax contains an uninhabited sort");
  return model;
}
}

ParseError::ParseError(std::string s, std::string_view text, std::size_t at, const std::string& reason)
    : std::invalid_argument(message(s, text, at, reason)), source(std::move(s)),
      offset(std::min(at, text.size())), line(location(text, at).first), column(location(text, at).second) {}
Language::Language(std::shared_ptr<const detail::Spec> spec) : spec_(std::move(spec)) {}
const std::string& Language::name() const { return spec_->model->name; }
Dialect Language::default_dialect() const { return spec_->model->requires_extended ? Dialect::Two : Dialect::One; }
Language parse(std::string_view text, std::string source) {
  if (text.size() > 65536) throw ParseError(source, text, 0, "specification exceeds 64 KiB");
  auto nul = text.find('\0');
  if (nul != std::string_view::npos) throw ParseError(source, text, nul, "embedded NUL");
  std::string input(text);
  std::unique_ptr<novo_as_syntax, decltype(&novo_as_syntax_free)> syntax(nullptr, novo_as_syntax_free);
  std::size_t offset = 0;
  {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    syntax.reset(novo_as_parse_syntax(input.c_str(), &offset));
  }
  if (!syntax) throw ParseError(source, text, offset, "invalid .acsem syntax or parser resource limit");
  auto spec = std::make_shared<detail::Spec>();
  try {
    spec->model = build_model(novo_as_syntax_document(syntax.get()), input, offset);
  } catch (const std::invalid_argument& e) { throw ParseError(source, text, offset, e.what()); }
  return Language(spec);
}
Target target(std::string_view n) {
  if (n == "c") return Target::C;
  if (n == "python") return Target::Python;
  if (n == "rust") return Target::Rust;
  if (n == "pascal") return Target::Pascal;
  if (n == "latex") return Target::Latex;
  throw std::invalid_argument("unknown target (expected c, python, rust, pascal, latex)");
}
} // namespace novostone::action
