#include "novostone/ActionSemantics/ActionSemantics.hpp"
#include "Model.hpp"
#include "ActionRuntimeSources.hpp"

#include <algorithm>
#include <iomanip>
#include <locale>
#include <limits>
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
  if (i.op == Arity) return "arity(" + a + ')';
  if (i.op >= Component && i.op <= Identical) {
    static constexpr const char* names[] = {"component", "equal", "identical"};
    return std::string(names[i.op - Component]) + '(' + a + ", " + expression(m, c, i.b) + ')';
  }
  static constexpr const char* ops[] = {"+", "-", "*", "/", "==", "<"};
  return '(' + a + ' ' + ops[i.op - Add] + ' ' + expression(m, c, i.b) + ')';
}
std::string pattern(const detail::Model& m, int id) {
  using namespace detail;
  const auto& p = m.patterns.at(static_cast<std::size_t>(id));
  if (p.op == AnyPattern) return "_";
  if (p.op == BindPattern) return m.tokens.at(static_cast<std::size_t>(p.a));
  if (p.op == IntPattern) return std::to_string(p.value);
  if (p.op == TokenPattern) return '"' + m.tokens.at(static_cast<std::size_t>(p.a)) + '"';
  if (p.op == AsPattern) return '(' + pattern(m, p.a) + ") as " + m.tokens.at(static_cast<std::size_t>(p.b));
  if (p.op == OrPattern) return '(' + pattern(m, p.a) + " | " + pattern(m, p.b) + ')';
  auto out = m.variants.at(static_cast<std::size_t>(p.a)).name + '(';
  for (int j = 0; j < p.count; ++j) {
    if (j) out += ", ";
    out += pattern(m, m.pool.at(static_cast<std::size_t>(p.pool + j)));
  }
  return out + ')';
}
std::string action(const detail::Model& m, const detail::Constructor& c, int id) {
  using namespace detail;
  const auto& i = m.actions.at(static_cast<std::size_t>(id));
  if (i.op == Rewrite) {
    static constexpr const char* names[] = {"reduce", "traverse topdown", "traverse bottomup",
      "traverse once", "saturate", "fixedpoint", "worklist", "merge"};
    const auto& visit = m.actions.at(static_cast<std::size_t>(i.b));
    auto out = std::string("rewrite ") + names[i.value] + " " + expression(m, c, i.a) + " using { ";
    for (int j = 0; j < visit.count; ++j) {
      if (j) out += ", ";
      auto base = static_cast<std::size_t>(visit.pool + 3 * j);
      out += pattern(m, m.pool.at(base));
      if (m.pool.at(base + 1) >= 0) out += " when " + expression(m, c, m.pool.at(base + 1));
      out += " => " + action(m, c, m.pool.at(base + 2));
    }
    return out + " }";
  }
  if (i.op == Construct) {
    auto out = "construct " + m.variants.at(static_cast<std::size_t>(i.a)).name + '(';
    for (int j = 0; j < i.count; ++j) {
      if (j) out += ", ";
      out += expression(m, c, m.pool.at(static_cast<std::size_t>(i.pool + j)));
    }
    return out + ')';
  }
  if (i.op == Deconstruct) return "deconstruct " + expression(m, c, i.a);
  if (i.op == Match) {
    auto out = "match " + expression(m, c, i.a);
    if (i.b >= FirstDataSort) out += " of " + m.data_sorts.at(static_cast<std::size_t>(i.b - FirstDataSort));
    out += " with { ";
    for (int j = 0; j < i.count; ++j) {
      if (j) out += ", ";
      auto base = static_cast<std::size_t>(i.pool + 3 * j);
      out += pattern(m, m.pool.at(base));
      if (m.pool.at(base + 1) >= 0) out += " when " + expression(m, c, m.pool.at(base + 1));
      out += " => " + action(m, c, m.pool.at(base + 2));
    }
    return out + " }";
  }
  if (i.op == 0) return "complete";
  if (i.op == 1) return "fail";
  if (i.op == 7) return "allocate cell";
  if (i.op == 8) return "evaluate(" + c.fields.at(static_cast<std::size_t>(i.a)).name + ')';
  if (i.op == 14) return "furthermore (" + action(m, c, i.a) + ')';
  if (i.op == 16) return "closure (" + action(m, c, i.a) + ')';
  if (i.op == 17 || i.op == 19) {
    auto out = std::string(i.op == 17 ? "apply " : "tailcall ") + expression(m, c, i.a) + " to (";
    for (int j = 0; j < i.count; ++j) {
      if (j) out += ", ";
      out += expression(m, c, m.pool.at(static_cast<std::size_t>(i.pool + j)));
    }
    return out + ")";
  }
  if (i.op == 18) return "return " + expression(m, c, i.a);
  if (i.op == 20) return "enact " + expression(m, c, i.a);
  if (i.op == detail::CallCC) return "callcc " + expression(m, c, i.a);
  if (i.op == detail::ResumeCont)
    return "resume " + expression(m, c, i.a) + " with " + expression(m, c, i.b);
  if (i.op == detail::Unfolding) return "unfolding (" + action(m, c, i.a) + ')';
  if (i.op == detail::Unfold) return "unfold";
  if (i.op == detail::TailUnfold) return "tailunfold";
  if (i.op == 102) return "put " + expression(m, c, i.a) + " in " + expression(m, c, i.b);
  if (i.op == 103)
    return "if " + expression(m, c, i.a) + " then (" + action(m, c, i.b) +
      ") else (" + action(m, c, static_cast<int>(i.value)) + ')';
  if (i.op == 104)
    return "while " + expression(m, c, i.a) + " do (" + action(m, c, i.b) + ')';
  if (i.op == 105 || i.op == 106) return i.op == 105 ? "break" : "continue";
  if (i.op == 110) return "generator (" + action(m, c, i.a) + ')';
  if (i.op >= 111 && i.op <= 113) {
    static constexpr const char* ops[] = {"yield_value ", "next ", "close "};
    return std::string(ops[i.op - 111]) + expression(m, c, i.a);
  }
  if (i.op == 21) {
    auto out = std::string("letrec ");
    for (int j = 0; j < i.count; ++j) {
      if (j) out += ", ";
      out += '"' + m.tokens.at(static_cast<std::size_t>(m.pool.at(static_cast<std::size_t>(i.pool + 2 * j)))) +
             "\" = closure (" + action(m, c, m.pool.at(static_cast<std::size_t>(i.pool + 2 * j + 1))) + ")";
    }
    return out + " in (" + action(m, c, i.b) + ")";
  }
  if (i.op >= 9) {
    static constexpr const char* ops[] = {"then", "and then", "hence", "otherwise", "trapping"};
    return '(' + action(m, c, i.a) + ' ' + ops[i.op - 9] + ' ' + action(m, c, i.b) + ')';
  }
  static constexpr const char* ops[] = {"give ", "check ", "escape ", "bind ", "store "};
  auto out = std::string(ops[i.op - 2]) + expression(m, c, i.a);
  if (i.op == 5 || i.op == 6) out += std::string(i.op == 5 ? " to " : " in ") + expression(m, c, i.b);
  return out;
}
std::string fingerprint(std::string_view s) {
  std::uint64_t hash = UINT64_C(14695981039346656037);
  for (unsigned char c : s) { hash ^= c; hash *= UINT64_C(1099511628211); }
  std::ostringstream out; out.imbue(std::locale::classic());
  out << std::hex << std::setw(16) << std::setfill('0') << hash; return out.str();
}
std::string stamped(std::string_view stamp, const char* dialect, bool py, bool pas, std::string_view output) {
  auto full = "acsem dialect " + std::string(dialect) + "; source FNV-1a-64 " + std::string(stamp);
  return (py ? "# " + full : pas ? "{ " + full + " }" : "/* " + full + " */") + '\n' + std::string(output);
}
std::string fields_of(const detail::Model& m, const detail::Constructor& ctor, std::size_t f) {
  const auto& field = ctor.fields[f];
  return latex(field.type == 0 ? "integer" : field.type == 1 ? "identifier" : m.sorts[static_cast<std::size_t>(field.type - 2)]);
}
// Both capability modes share one instruction format and Action Machine.
std::string generate_model(const detail::Model& m, Target target, Dialect dialect) {
  const bool c = target == Target::C, py = target == Target::Python, rs = target == Target::Rust, pas = target == Target::Pascal;
  const auto& b = m.budgets;
  const char* version = dialect == Dialect::One ? "1" : "2";
  const std::int64_t lo = m.int_width == 64 ? INT64_MIN : -2147483648LL;
  const std::int64_t hi = m.int_width == 64 ? INT64_MAX : 2147483647LL;
  const std::uint64_t mag = m.int_width == 64 ? UINT64_C(9223372036854775808) : UINT64_C(2147483648);
  std::ostringstream out; out.imbue(std::locale::classic());
  if (target == Target::Latex) {
    out << "% acsem dialect " << version << "; source FNV-1a-64 " << fingerprint(m.source) << "\n"
        << "\\documentclass{article}\n\\usepackage[T1]{fontenc}\n\\usepackage{longtable}\n"
        << "\\begin{document}\n\\title{Action semantics of " << latex(m.name) << "}\n"
        << "\\author{Novostone ActionSemantics}\n\\date{}\n\\maketitle\n"
        << "Executable dialect " << version << " on the continuation-based Action Machine, based on the action semantics framework of Peter D. Mosses and David A. Watt.\n"
        << "\\section{Abstract syntax}\nStart sort: \\texttt{" << latex(m.sorts[static_cast<std::size_t>(m.start)]) << "}.\n"
        << "\\begin{longtable}{p{0.20\\linewidth}p{0.72\\linewidth}}\n";
    for (const auto& ctor : m.constructors) {
      out << "\\texttt{" << latex(m.sorts[static_cast<std::size_t>(ctor.sort)]) << "} & \\texttt{" << latex(ctor.name) << '(';
      for (std::size_t f = 0; f < ctor.fields.size(); ++f) {
        if (f) out << ", ";
        out << latex(ctor.fields[f].name) << ": " << fields_of(m, ctor, f);
      }
      out << ")} \\\\\n";
    }
    out << "\\end{longtable}\n";
    if (!m.data_sorts.empty()) {
      out << "\\section{Runtime data sorts}\n\\begin{longtable}{p{0.20\\linewidth}p{0.72\\linewidth}}\n";
      for (const auto& v : m.variants) {
        out << "\\texttt{" << latex(m.data_sorts[v.sort - detail::FirstDataSort]) << "} & \\texttt{" << latex(v.name) << '(';
        for (int j = 0; j < v.arity; ++j) {
          if (j) out << ", ";
          int t = m.pool[v.fields + j];
          out << latex(t == 0 ? "integer" : t == 1 ? "identifier" : t == 2 ? "datum" :
                       m.data_sorts[t - detail::FirstDataSort]);
        }
        out << ")} \\\\\n";
      }
      out << "\\end{longtable}\n";
    }
    out << "\\section{Semantic equations}\n"
        << "\\begin{longtable}{p{0.20\\linewidth}p{0.72\\linewidth}}\n";
    for (const auto& ctor : m.constructors) {
      out << "$[\\![\\texttt{" << latex(ctor.name) << '(';
      for (std::size_t f = 0; f < ctor.fields.size(); ++f) {
        if (f) out << ", ";
        out << latex(ctor.fields[f].name);
      }
      out << ")}]\\!]$ & $=$ \\texttt{" << latex(action(m, ctor, ctor.rule)) << "} \\\\\n";
    }
    out << "\\end{longtable}\n\\section{Execution contract}\n"
        << "Actions complete, fail, or escape on an explicit continuation-based Action Machine whose frames are heap objects. "
        << "Yielders inspect transient data, bindings, and storage. Integers are checked signed "
        << (m.int_width == 64 ? "64" : "32") << "-bit values; tokens and cells are distinct tagged data. "
        << "The deterministic otherwise combinator handles failure without rolling back storage. "
        << "Environments and transient tuples are managed by tracing garbage collection; cell storage is monotonic like dialect 1. "
        << "Resource exhaustion is reported separately from semantic failure. "
        << "Closures capture lexical bindings and their defining syntax node. Application evaluates the function and arguments left-to-right. "
        << "Tail calls replace the current function continuation. Callcc captures one-shot abortive continuations; resumption shares mutable storage. "
        << "Unfolding delimits recursive actions. If selects one branch; while repeats until its integer condition is zero, with scoped break and continue. "
        << "Generators are lazy saved-frame computations. Yield suspends at next; next returns flag 1 followed by the yielded value, "
        << "or flag 0 followed by completion data. Close discards suspended frames. Generator failure and escape propagate. "
        << "Data constructors check field tags and build immutable values. Match tries patterns and guards in order, "
        << "with captures scoped to the selected arm. Equal compares structure; identical compares identity. "
        << "Callcc and generators cannot yet be combined in one specification. No asynchronous scheduler or cleanup handlers are implemented. "
        << "The generated interpreter reads constructor-form syntax from standard input.\n"
        << "\\end{document}\n";
    return out.str();
  }
  if (c) {
    out << "enum { START = " << m.start << ", STEPS_MAX = " << b.steps << ", NODES_MAX = " << b.nodes
        << ", LEGACY_MODE = " << (dialect == Dialect::One ? 1 : 0)
        << ", SYMBOLS_MAX = " << b.symbols << ", REFS_MAX = " << b.refs << ", DATA_MAX = " << b.data_items
        << ", BIND_MAX = " << b.bindings << ", DEPTH_MAX = " << b.depth << ", PARSE_DEPTH_MAX = " << b.parse_depth
        << ", HEAP_MAX = " << b.heap << ", TOKEN_COUNT = " << m.tokens.size()
        << ", CTOR_COUNT = " << m.constructors.size() << " };\n"
        << "static const int64_t INT_LO = " << lo << "LL, INT_HI = " << hi << "LL;\n"
        << "static const uint64_t INT_MAG = " << mag << "ULL;\n"
        << "static const Constructor CTORS[] = {\n";
  }
  if (py) {
    out << "START = " << m.start << "\n"
        << "LEGACY_MODE = " << (dialect == Dialect::One ? "True" : "False") << "\n"
        << "INT_LO, INT_HI, INT_MAG = " << lo << ", " << hi << ", " << mag << "\n"
        << "STEPS_MAX = " << b.steps << "\nNODES_MAX = " << b.nodes << "\nSYMBOLS_MAX = " << b.symbols
        << "\nREFS_MAX = " << b.refs << "\nDATA_MAX = " << b.data_items << "\nBIND_MAX = " << b.bindings
        << "\nDEPTH_MAX = " << b.depth << "\nPARSE_DEPTH_MAX = " << b.parse_depth << "\nHEAP_MAX = " << b.heap << "\n"
        << "CTORS = [\n";
  }
  if (rs) {
    out << "const START: i32 = " << m.start << ";\n"
        << "const LEGACY_MODE: bool = " << (dialect == Dialect::One ? "true" : "false") << ";\n"
        << "const INT_LO: i64 = " << lo << ";\nconst INT_HI: i64 = " << hi << ";\nconst INT_MAG: i64 = " << static_cast<std::int64_t>(mag) << ";\n"
        << "const STEPS_MAX: i64 = " << b.steps << ";\nconst NODES_MAX: usize = " << b.nodes
        << ";\nconst SYMBOLS_MAX: usize = " << b.symbols << ";\nconst REFS_MAX: usize = " << b.refs
        << ";\nconst DATA_MAX: usize = " << b.data_items << ";\nconst BIND_MAX: usize = " << b.bindings
        << ";\nconst DEPTH_MAX: usize = " << b.depth << ";\nconst PARSE_DEPTH_MAX: usize = " << b.parse_depth
        << ";\nconst HEAP_MAX: usize = " << b.heap << ";\n"
        << "const CTORS: &[Ctor] = &[\n";
  }
  if (pas) {
    out << "  StartSort = " << m.start << ";\n"
        << "  LegacyMode = " << (dialect == Dialect::One ? "True" : "False") << ";\n"
        << "  IntLo: Int64 = " << lo << ";\n  IntHi: Int64 = " << hi << ";\n  IntMag: Int64 = " << static_cast<std::int64_t>(mag) << ";\n"
        << "  StepsMax: Int64 = " << b.steps << ";\n  NodesMax = " << b.nodes << ";\n  SymbolsMax = " << b.symbols
        << ";\n  RefsMax = " << b.refs << ";\n  DataMax = " << b.data_items << ";\n  BindMax = " << b.bindings
        << ";\n  DepthMax = " << b.depth << ";\n  ParseDepthMax = " << b.parse_depth << ";\n  HeapMax = " << b.heap << ";\n"
        << "  CtorCount = " << m.constructors.size() << ";\n  TokenCount = " << m.tokens.size() << ";\n"
        << "  Ctors: array[0.." << m.constructors.size() - 1 << "] of TConstructor = (\n";
  }
  for (std::size_t j = 0; j < m.constructors.size(); ++j) {
    const auto& ctor = m.constructors[j];
    if (c) out << "{\"" << ctor.name << "\", " << ctor.sort << ", " << ctor.rule << ", " << ctor.fields.size() << ", {";
    if (py) out << "('" << ctor.name << "', " << ctor.sort << ", " << ctor.rule << ", [";
    if (rs) out << "Ctor { name: \"" << ctor.name << "\", sort: " << ctor.sort << ", rule: " << ctor.rule << ", fields: &[";
    if (pas) out << "  (Name: '" << ctor.name << "'; Sort: " << ctor.sort << "; Rule: " << ctor.rule << "; Count: " << ctor.fields.size() << "; Fields: (";
    auto count = pas ? std::size_t{16} : std::max(ctor.fields.size(), c ? std::size_t{1} : std::size_t{0});
    for (std::size_t f = 0; f < count; ++f) {
      if (f) out << ", ";
      out << (f < ctor.fields.size() ? ctor.fields[f].type : 0);
    }
    out << (c ? "}}" : py ? "])" : rs ? "] }" : "))");
    out << (j + 1 == m.constructors.size() ? "\n" : ",\n");
  }
  out << (c ? "};\n" : py ? "]\n" : rs ? "];\n" : ");\n");
  if (c) out << "static const char *const TOKENS[] = {";
  if (py) out << "TOKENS = [";
  if (rs) out << "const TOKENS: &[&str] = &[";
  if (pas) out << "  Tokens: array[0.." << std::max(m.tokens.size(), std::size_t{1}) - 1 << "] of string[64] = (";
  {
    auto count = std::max(m.tokens.size(), (c || pas) ? std::size_t{1} : std::size_t{0});
    for (std::size_t j = 0; j < count; ++j) {
      if (j) out << ", ";
      out << (pas ? "'" : "\"") << (j < m.tokens.size() ? m.tokens[j] : "") << (pas ? "'" : "\"");
    }
  }
  out << (c ? "};\n" : py ? "]\n" : rs ? "];\n" : ");\n");
  auto instructions = [&](const auto& code, const char* name) {
    if (c) out << "static const Instruction " << name << "[] = {\n";
    if (py) out << name << " = [\n";
    if (rs) out << "const " << name << ": &[Inst] = &[\n";
    if (pas) out << "  " << name << ": array[0.." << std::max(code.size(), std::size_t{1}) - 1 << "] of TInstruction = (\n";
    auto n = std::max(code.size(), (c || pas) ? std::size_t{1} : std::size_t{0});
    for (std::size_t j = 0; j < n; ++j) {
      detail::Instruction i = j < code.size() ? code[j] : detail::Instruction{};
      if (c) out << "{" << i.op << ", " << i.a << ", " << i.b << ", " << i.value << ", " << i.pool << ", " << i.count << "}";
      if (py) out << "(" << i.op << ", " << i.a << ", " << i.b << ", " << i.value << ", " << i.pool << ", " << i.count << ")";
      if (rs) out << "Inst { op: " << i.op << ", a: " << i.a << ", b: " << i.b << ", value: " << i.value << ", pool: " << i.pool << ", count: " << i.count << " }";
      if (pas) out << "(Op: " << i.op << "; A: " << i.a << "; B: " << i.b << "; Pool: " << i.pool << "; Count: " << i.count << "; Number: " << i.value << ')';
      out << (j + 1 == n ? "\n" : ",\n");
    }
    out << (c ? "};\n" : py ? "]\n" : rs ? "];\n" : ");\n");
  };
  instructions(m.expressions, "EXPRS"); instructions(m.actions, "ACTIONS");
  instructions(m.patterns, "PATTERNS");
  if (c) out << "static const Variant VARIANTS[] = {\n";
  if (py) out << "VARIANTS = [\n";
  if (rs) out << "const VARIANTS: &[Variant] = &[\n";
  if (pas) out << "  Variants: array[0.." << std::max(m.variants.size(), std::size_t{1}) - 1 << "] of TVariant = (\n";
  for (std::size_t j = 0, n = std::max(m.variants.size(), (c || pas) ? std::size_t{1} : std::size_t{0}); j < n; ++j) {
    auto v = j < m.variants.size() ? m.variants[j] : detail::VariantSpec{"", -1, 0, 0};
    if (c || py) out << (c ? "{" : "(") << '"' << v.name << "\", " << v.sort << ", " << v.fields << (c ? "}" : ")");
    if (rs) out << "Variant { name: \"" << v.name << "\", sort: " << v.sort << ", fields: " << v.fields << " }";
    if (pas) out << "(Name: '" << v.name << "'; Sort: " << v.sort << "; Fields: " << v.fields << ')';
    out << (j + 1 == n ? "\n" : ",\n");
  }
  out << (c ? "};\n" : py ? "]\n" : rs ? "];\n" : ");\n");
  {
    if (c) out << "static const int POOL[] = {";
    if (py) out << "POOL = [";
    if (rs) out << "const POOL: &[i32] = &[";
    if (pas) out << "  Pool: array[0.." << std::max(m.pool.size(), std::size_t{1}) - 1 << "] of LongInt = (";
    auto n = std::max(m.pool.size(), (c || pas) ? std::size_t{1} : std::size_t{0});
    for (std::size_t j = 0; j < n; ++j) {
      if (j) out << ", ";
      out << (j < m.pool.size() ? m.pool[j] : 0);
    }
    out << (c ? "};\n" : py ? "]\n" : rs ? "];\n" : ");\n");
  }
  std::string output(c ? detail::runtime_c : py ? detail::runtime_python : rs ? detail::runtime_rust : detail::runtime_pascal);
  output.replace(output.find("@TABLES@"), 8, out.str());
  return stamped(fingerprint(m.source), version, py, pas, output);
}
}
std::string generate(const Language& language, Target target) {
  Options options;
  options.dialect = language.default_dialect();
  return generate(language, target, options);
}
std::string generate(const Language& language, Target target, const Options& options) {
  auto checked_budget = [](std::int64_t value, std::int64_t cap, const char* key) {
    if (value < 0 || value > cap)
      throw std::invalid_argument(std::string("budget ") + key + " must be in 0.." + std::to_string(cap));
  };
  checked_budget(options.steps, std::numeric_limits<int>::max(), "steps");
  checked_budget(options.nodes, 65536, "nodes");
  checked_budget(options.symbols, 4096, "symbols");
  checked_budget(options.refs, 65536, "refs");
  checked_budget(options.depth, 4096, "depth");
  checked_budget(options.heap, 65536, "heap");
  const bool overrides = options.steps || options.nodes || options.symbols ||
    options.refs || options.depth || options.heap;
  if (options.dialect == Dialect::One && overrides)
    throw std::invalid_argument("budget overrides require dialect 2");
  auto m = *language.spec_->model;
  if (options.dialect == Dialect::One && m.requires_extended)
    throw std::invalid_argument("specification uses dialect-2 constructs; synthesize with --dialect 2");
  bool continuations = false, generators = false;
  for (const auto& i : m.actions) {
    continuations |= i.op == detail::CallCC || i.op == detail::ResumeCont;
    generators |= i.op >= 110 && i.op <= 113;
  }
  if (continuations && generators)
    throw std::invalid_argument("capability error: callcc/resume and generators cannot yet share a specification");
  if (options.steps) m.budgets.steps = options.steps;
  if (options.nodes) m.budgets.nodes = options.nodes;
  if (options.symbols) m.budgets.symbols = options.symbols;
  if (options.refs) m.budgets.refs = options.refs;
  if (options.depth) m.budgets.depth = options.depth;
  if (options.heap) m.budgets.heap = options.heap;
  if (m.budgets.symbols < static_cast<int>(m.tokens.size()))
    throw std::invalid_argument("symbol budget is smaller than the specification token table");
  return generate_model(m, target, options.dialect);
}
} // namespace novostone::action
