#include "novostone/ActionSemantics/ActionSemantics.hpp"
#include "Model.hpp"
#include "ActionRuntimeSources.hpp"

#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>

namespace novostone::action {
namespace {
std::string latex(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\textbackslash{}"; break;
      case '{': case '}': case '$': case '&': case '#': case '_': case '%': out += '\\'; out += c; break;
      case '~': out += "\\textasciitilde{}"; break;
      case '^': out += "\\textasciicircum{}"; break;
      case '<': out += "\\textless{}"; break;
      case '>': out += "\\textgreater{}"; break;
      default: out += c; break;
    }
  }
  return out;
}
std::string expression(const detail::Model& m, const detail::Constructor& c, int id) {
  using namespace detail;
  const auto& i = m.expressions.at(static_cast<std::size_t>(id));
  if (i.op == Integer) return std::to_string(i.value);
  if (i.op == Field) return c.fields.at(static_cast<std::size_t>(i.a)).name;
  if (i.op == Token) return '"' + m.tokens.at(static_cast<std::size_t>(i.a)) + '"';
  if (i.op == Given) return "given(" + std::to_string(i.a + 1) + ')';
  auto a = expression(m, c, i.a);
  if (i.op == Bound || i.op == Contents) return std::string(i.op == Bound ? "bound(" : "contents(") + a + ')';
  static constexpr const char* ops[] = {"+", "-", "*", "/", "==", "<"};
  return '(' + a + ' ' + ops[i.op - Add] + ' ' + expression(m, c, i.b) + ')';
}
std::string action(const detail::Model& m, const detail::Constructor& c, int id) {
  using namespace detail;
  const auto& i = m.actions.at(static_cast<std::size_t>(id));
  if (i.op == Complete) return "complete";
  if (i.op == Fail) return "fail";
  if (i.op == Allocate) return "allocate cell";
  if (i.op == Evaluate) return "evaluate(" + c.fields.at(static_cast<std::size_t>(i.a)).name + ')';
  if (i.op == Furthermore) return "furthermore (" + action(m, c, i.a) + ')';
  if (i.op >= Then) {
    static constexpr const char* ops[] = {"then", "and then", "hence", "otherwise", "trapping"};
    return '(' + action(m, c, i.a) + ' ' + ops[i.op - Then] + ' ' + action(m, c, i.b) + ')';
  }
  static constexpr const char* ops[] = {"give ", "check ", "escape ", "bind ", "store "};
  auto out = std::string(ops[i.op - Give]) + expression(m, c, i.a);
  if (i.op == Bind || i.op == Store) out += std::string(i.op == Bind ? " to " : " in ") + expression(m, c, i.b);
  return out;
}
std::string fingerprint(std::string_view s) {
  std::uint64_t hash = UINT64_C(14695981039346656037);
  for (unsigned char c : s) { hash ^= c; hash *= UINT64_C(1099511628211); }
  std::ostringstream out; out.imbue(std::locale::classic());
  out << std::hex << std::setw(16) << std::setfill('0') << hash; return out.str();
}
}
std::string generate(const Language& language, Target target) {
  const auto& m = *language.model_;
  std::ostringstream out; out.imbue(std::locale::classic());
  if (target == Target::Latex) {
    out << "% acsem dialect 1; source FNV-1a-64 " << fingerprint(m.source) << "\n"
        << "\\documentclass{article}\n\\usepackage[T1]{fontenc}\n\\usepackage{longtable}\n"
        << "\\begin{document}\n\\title{Action semantics of " << latex(m.name) << "}\n"
        << "\\author{Novostone ActionSemantics}\n\\date{}\n\\maketitle\n"
        << "Executable dialect 1, based on the action semantics framework of Peter D. Mosses and David A. Watt.\n"
        << "\\section{Abstract syntax}\nStart sort: \\texttt{" << latex(m.sorts[static_cast<std::size_t>(m.start)]) << "}.\n"
        << "\\begin{longtable}{p{0.20\\linewidth}p{0.72\\linewidth}}\n";
    for (const auto& c : m.constructors) {
      out << "\\texttt{" << latex(m.sorts[static_cast<std::size_t>(c.sort)]) << "} & \\texttt{" << latex(c.name) << '(';
      for (std::size_t f = 0; f < c.fields.size(); ++f) {
        if (f) out << ", ";
        const auto& field = c.fields[f];
        out << latex(field.name) << ": " << latex(field.type == 0 ? "integer" : field.type == 1 ? "identifier" : m.sorts[static_cast<std::size_t>(field.type - 2)]);
      }
      out << ")} \\\\\n";
    }
    out << "\\end{longtable}\n\\section{Semantic equations}\n"
        << "\\begin{longtable}{p{0.20\\linewidth}p{0.72\\linewidth}}\n";
    for (const auto& c : m.constructors) {
      out << "$[\\![\\texttt{" << latex(c.name) << '(';
      for (std::size_t f = 0; f < c.fields.size(); ++f) {
        if (f) out << ", ";
        out << latex(c.fields[f].name);
      }
      out << ")}]\\!]$ & $=$ \\texttt{" << latex(action(m, c, c.rule)) << "} \\\\\n";
    }
    out << "\\end{longtable}\n\\section{Execution contract}\n"
        << "Actions complete, fail, or escape. Yielders inspect transient data, bindings, and storage. "
        << "Integers are checked signed 32-bit values; tokens and cells are distinct tagged data. "
        << "The deterministic otherwise combinator handles failure without rolling back storage. "
        << "Resource exhaustion is reported separately from semantic failure. "
        << "The generated interpreter reads constructor-form syntax from standard input.\n"
        << "\\end{document}\n";
    return out.str();
  }
  const bool c = target == Target::C, py = target == Target::Python, rs = target == Target::Rust, pas = target == Target::Pascal;
  if (!c && !py && !rs && !pas) throw std::invalid_argument("invalid synthesis target");
  if (c) out << "enum { START = " << m.start << ", CTOR_COUNT = " << m.constructors.size() << ", TOKEN_COUNT = " << m.tokens.size() << " };\n";
  if (py) out << "START = " << m.start << "\n";
  if (rs) out << "const START: i32 = " << m.start << ";\n";
  if (pas) out << "const StartSort = " << m.start << "; CtorCount = " << m.constructors.size() << "; TokenCount = " << m.tokens.size() << ";\n";
  if (c) out << "static const Constructor CTORS[] = {\n";
  if (py) out << "CTORS = [\n";
  if (rs) out << "const CTORS: &[Ctor] = &[\n";
  if (pas) out << "Ctors: array[0.." << m.constructors.size() - 1 << "] of TConstructor = (\n";
  for (std::size_t j = 0; j < m.constructors.size(); ++j) {
    const auto& ctor = m.constructors[j];
    if (c) out << "{\"" << ctor.name << "\", " << ctor.sort << ", " << ctor.rule << ", " << ctor.fields.size() << ", {";
    if (py) out << "(\"" << ctor.name << "\", " << ctor.sort << ", " << ctor.rule << ", [";
    if (rs) out << "Ctor { name: \"" << ctor.name << "\", sort: " << ctor.sort << ", rule: " << ctor.rule << ", fields: &[";
    if (pas) out << "(Name: '" << ctor.name << "'; Sort: " << ctor.sort << "; Rule: " << ctor.rule << "; Count: " << ctor.fields.size() << "; Fields: (";
    auto count = pas ? std::size_t{16} : std::max(ctor.fields.size(), c ? std::size_t{1} : std::size_t{0});
    for (std::size_t f = 0; f < count; ++f) { if (f) out << ", "; out << (f < ctor.fields.size() ? ctor.fields[f].type : 0); }
    out << (c ? "}}" : py ? "])" : rs ? "] }" : "))");
    out << (j + 1 == m.constructors.size() ? "\n" : ",\n");
  }
  out << (c ? "};\n" : py ? "]\n" : rs ? "];\n" : ");\n");
  if (c) out << "static const char *const TOKENS[] = {";
  if (py) out << "TOKENS = [";
  if (rs) out << "const TOKENS: &[&str] = &[";
  if (pas) out << "Tokens: array[0.." << std::max(m.tokens.size(), std::size_t{1}) - 1 << "] of string[64] = (";
  auto count = std::max(m.tokens.size(), (c || pas) ? std::size_t{1} : std::size_t{0});
  for (std::size_t j = 0; j < count; ++j) {
    if (j) out << ", ";
    out << (pas ? "'" : "\"") << (j < m.tokens.size() ? m.tokens[j] : "") << (pas ? "'" : "\"");
  }
  out << (c ? "};\n" : py ? "]\n" : rs ? "];\n" : ");\n");
  auto instructions = [&](const auto& code, const char* name) {
    if (c) out << "static const Instruction " << name << "[] = {\n";
    if (py) out << name << " = [\n";
    if (rs) out << "const " << name << ": &[Inst] = &[\n";
    if (pas) out << name << ": array[0.." << std::max(code.size(), std::size_t{1}) - 1 << "] of TInstruction = (\n";
    auto n = std::max(code.size(), (c || pas) ? std::size_t{1} : std::size_t{0});
    for (std::size_t j = 0; j < n; ++j) {
      detail::Instruction i = j < code.size() ? code[j] : detail::Instruction{};
      if (c || py) out << (c ? "{" : "(") << i.op << ", " << i.a << ", " << i.b << ", " << i.value << (c ? "}" : ")");
      if (rs) out << "Inst { op: " << i.op << ", a: " << i.a << ", b: " << i.b << ", value: " << i.value << " }";
      if (pas) out << "(Op: " << i.op << "; A: " << i.a << "; B: " << i.b << "; Number: " << i.value << ')';
      out << (j + 1 == n ? "\n" : ",\n");
    }
    out << (c ? "};\n" : py ? "]\n" : rs ? "];\n" : ");\n");
  };
  instructions(m.expressions, "EXPRS"); instructions(m.actions, "ACTIONS");
  std::string output(c ? detail::runtime_c : py ? detail::runtime_python : rs ? detail::runtime_rust : detail::runtime_pascal);
  output.replace(output.find("@TABLES@"), 8, out.str());
  auto stamp = "acsem dialect 1; source FNV-1a-64 " + fingerprint(m.source);
  return (py ? "# " + stamp : pas ? "{ " + stamp + " }" : "/* " + stamp + " */") + '\n' + output;
}
} // namespace novostone::action
