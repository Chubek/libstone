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
  detail::Model& model;
  const detail::Constructor& ctor;
  std::size_t& offset;
  std::map<novo_as_expr_t, int> expressions;
  std::map<novo_as_action_t, int> actions;
  std::map<novo_as_expr_t, int> types; // integer, token, cell, dynamic datum
  int field(const std::string& n) const {
    for (std::size_t i = 0; i < ctor.fields.size(); ++i)
      if (ctor.fields[i].name == n) return static_cast<int>(i);
    throw std::invalid_argument("unknown field '" + n + "' in " + ctor.name);
  }
  void expect(novo_as_expr_t e, int type) {
    require(types.at(e) == type || types.at(e) == 3, "incompatible yielder type");
  }
};
int lower_expr(Lower* c, novo_as_expr_t e) {
  using namespace detail;
  c->offset = e->offset;
  Instruction i;
  int type = 0;
  switch (e->kind) {
    case novo_as_Integer_kind: {
      auto s = str(e->v.Integer.spelling);
      auto p = std::from_chars(s.data(), s.data() + s.size(), i.value);
      require(p.ec == std::errc{} && p.ptr == s.data() + s.size() &&
        i.value >= -2147483648LL && i.value <= 2147483647LL, "integer outside signed 32-bit range");
      i.op = Integer; break;
    }
    case novo_as_Field_kind:
      i.op = Field; i.a = c->field(str(e->v.Field.name));
      type = c->ctor.fields[static_cast<std::size_t>(i.a)].type;
      require(type < 2, "syntax fields must be used with evaluate(field)"); break;
    case novo_as_Token_kind: {
      auto s = str(e->v.Token.name); s = s.substr(1, s.size() - 2); name(s);
      auto it = std::find(c->model.tokens.begin(), c->model.tokens.end(), s);
      i.op = Token; type = 1; i.a = static_cast<int>(it - c->model.tokens.begin());
      if (it == c->model.tokens.end()) c->model.tokens.push_back(s);
      require(c->model.tokens.size() <= 256, "token limit exceeded"); break;
    }
    case novo_as_Unary_kind: {
      auto& u = e->v.Unary; auto op = str(u.op); i.a = c->expressions.at(u.operand);
      if (op == "given") {
        const auto& n = c->model.expressions[static_cast<std::size_t>(i.a)];
        require(n.op == Integer && n.value >= 1 && n.value <= 64, "given index must be in 1..64");
        i.op = Given; i.a = static_cast<int>(n.value - 1); type = 3;
      } else if (op == "bound") { c->expect(u.operand, 1); i.op = Bound; type = 3; }
      else { c->expect(u.operand, 2); i.op = Contents; }
      break;
    }
    case novo_as_Binary_kind: {
      auto& b = e->v.Binary; c->expect(b.left, 0); c->expect(b.right, 0);
      static const std::map<std::string, int> ops{{"+", Add}, {"-", Sub}, {"*", Mul}, {"/", Div}, {"==", Equal}, {"<", Less}};
      i.op = ops.at(str(b.op)); i.a = c->expressions.at(b.left); i.b = c->expressions.at(b.right); break;
    }
  }
  require(c->model.expressions.size() + c->model.actions.size() < 4096, "instruction limit exceeded");
  c->expressions[e] = static_cast<int>(c->model.expressions.size()); c->types[e] = type;
  c->model.expressions.push_back(i); return 0;
}
int lower_action(Lower* c, novo_as_action_t a) {
  using namespace detail;
  c->offset = a->offset;
  Instruction i;
  if (a->kind == novo_as_Evaluate_kind) {
    i.op = Evaluate; i.a = c->field(str(a->v.Evaluate.field));
    require(c->ctor.fields[static_cast<std::size_t>(i.a)].type >= 2, "evaluate requires a syntax field");
  } else if (a->kind == novo_as_Furthermore_kind) {
    i.op = Furthermore; i.a = c->actions.at(a->v.Furthermore.body);
  } else if (a->kind == novo_as_Combine_kind) {
    auto& b = a->v.Combine;
    static const std::map<std::string, int> ops{{"then", Then}, {"and then", AndThen}, {"hence", Hence},
      {"otherwise", Otherwise}, {"trapping", Trapping}};
    i.op = ops.at(str(b.op)); i.a = c->actions.at(b.left); i.b = c->actions.at(b.right);
  } else {
    auto& p = a->v.Primitive;
    static const std::map<std::string, int> ops{{"complete", Complete}, {"fail", Fail}, {"give", Give},
      {"check", Check}, {"escape", Escape}, {"bind", Bind}, {"store", Store}, {"allocate", Allocate}};
    i.op = ops.at(str(p.op));
    if (p.operands.length) i.a = c->expressions.at(p.operands.items[0]);
    if (p.operands.length > 1) i.b = c->expressions.at(p.operands.items[1]);
    if (i.op == Check || i.op == Store) c->expect(p.operands.items[0], 0);
    if (i.op == Bind) c->expect(p.operands.items[0], 1);
    if (i.op == Store) c->expect(p.operands.items[1], 2);
  }
  require(c->model.expressions.size() + c->model.actions.size() < 4096, "instruction limit exceeded");
  c->actions[a] = static_cast<int>(c->model.actions.size()); c->model.actions.push_back(i); return 0;
}
#define NOVO_AS_VISITOR_NAME lower
#define NOVO_AS_VISITOR_CONTEXT Lower
#define NOVO_AS_LEAVE_expr lower_expr
#define NOVO_AS_LEAVE_action lower_action
#define NOVO_AS_VISITOR_MAX_DEPTH 128
#include "ActionSemanticsVisitor.h"
#undef NOVO_AS_VISITOR_NAME
#undef NOVO_AS_VISITOR_CONTEXT
#undef NOVO_AS_LEAVE_expr
#undef NOVO_AS_LEAVE_action
#undef NOVO_AS_VISITOR_MAX_DEPTH
}

ParseError::ParseError(std::string s, std::string_view text, std::size_t at, const std::string& reason)
    : std::invalid_argument(message(s, text, at, reason)), source(std::move(s)),
      offset(std::min(at, text.size())), line(location(text, at).first), column(location(text, at).second) {}
Language::Language(std::shared_ptr<const detail::Model> model) : model_(std::move(model)) {}
const std::string& Language::name() const { return model_->name; }
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
  auto d = novo_as_syntax_document(syntax.get());
  auto model = std::make_shared<detail::Model>();
  model->name = str(d->name); model->source = input;
  try {
    name(model->name);
    std::map<std::string, int> sorts, constructors;
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
        auto c = s->constructors.items[k]; offset = c->offset;
        detail::Constructor ctor{str(c->name), static_cast<int>(j), -1, {}}; name(ctor.name);
        require(constructors.emplace(ctor.name, static_cast<int>(model->constructors.size())).second,
          "duplicate constructor: " + ctor.name);
        require(c->fields.length <= 16, "constructor exceeds 16 fields");
        std::set<std::string> fields;
        for (std::size_t f = 0; f < c->fields.length; ++f) {
          auto field = c->fields.items[f]; offset = field->offset;
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
      auto r = d->rules.items[j]; offset = r->offset; auto n = str(r->name);
      require(constructors.count(n), "rule for unknown constructor: " + n);
      auto& ctor = model->constructors[static_cast<std::size_t>(constructors.at(n))];
      require(ctor.rule == -1, "duplicate rule: " + n);
      Lower lowerer{*model, ctor, offset, {}, {}, {}};
      require(lower_action(r->body, &lowerer) == 0, "action exceeds visitor depth limit (128)");
      ctor.rule = lowerer.actions.at(r->body);
    }
    offset = 0;
    for (const auto& c : model->constructors) require(c.rule >= 0, "missing rule: " + c.name);
    std::set<int> productive;
    bool changed;
    do {
      changed = false;
      for (const auto& c : model->constructors) {
        bool ready = std::all_of(c.fields.begin(), c.fields.end(), [&](const auto& f) { return f.type < 2 || productive.count(f.type - 2); });
        if (ready) changed |= productive.insert(c.sort).second;
      }
    } while (changed);
    require(productive.size() == model->sorts.size(), "syntax contains an uninhabited sort");
  } catch (const std::invalid_argument& e) { throw ParseError(source, text, offset, e.what()); }
  return Language(std::move(model));
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
