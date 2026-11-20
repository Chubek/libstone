// ldscript.cpp - linker-script execution (layout driven by SECTIONS/MEMORY).
//
// Evaluates the AST produced by the parser in ldscript_parser.cpp and assigns
// output VMAs/file offsets plus symbol addresses. Also handles ENTRY,
// SEARCH_DIR and GROUP/INPUT file lists.
#include "internal.hpp"
#include "parser.hpp"
#include "ScriptSDK.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <limits>
#include <sstream>
#include <iomanip>

namespace qld::detail {

namespace {

uint64_t alignUp(uint64_t v, uint64_t a) {
  if (a <= 1) return v;
  if (v > UINT64_MAX - (a - 1)) throw std::runtime_error("script alignment overflow");
  return ((v + a - 1) / a) * a;
}

// Simple glob: '*' matches any run, '?' matches one char.
bool globMatch(const std::string& pat, const std::string& s) {
  size_t p = 0, t = 0, star = std::string::npos, mark = 0;
  while (t < s.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == s[t])) {
      ++p;
      ++t;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

bool fileMatches(const std::string& pattern, const std::string& label) {
  if (pattern == "*" || pattern.empty()) return true;
  // Labels look like "lib.a(member.o)" for archive members; match against
  // the full label, the basename, and the member name.
  std::string base = label;
  auto slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  std::string member = label;
  auto lp = label.find('(');
  if (lp != std::string::npos) {
    member = label.substr(lp + 1);
    if (!member.empty() && member.back() == ')') member.pop_back();
  }
  // Archive-qualified patterns like "lib.a:member.o" or "lib.a(member.o)".
  if (globMatch(pattern, label) || globMatch(pattern, base) ||
      globMatch(pattern, member))
    return true;
  // Suffix match for patterns like "*crt*.o".
  return false;
}

bool inputMatches(const LinkState& state, const script::SectionPattern& pattern,
                  const InputSection& input) {
  return fileMatches(pattern.file, state.objects[input.objectIndex].label) &&
         globMatch(pattern.section, input.section.name);
}

// Only DATA expressions with no address dependence can be mixed with GC.
// Other expressions govern layout, and names they read root input definitions.
bool collectExpressionRoots(const script::Expr* expr, std::set<std::string>& roots) {
  using namespace script;
  if (!expr || dynamic_cast<const NumberExpr*>(expr)) return true;
  if (auto* symbol = dynamic_cast<const SymbolExpr*>(expr)) roots.insert(symbol->name);
  else if (auto* unary = dynamic_cast<const UnaryExpr*>(expr))
    return collectExpressionRoots(unary->arg.get(), roots);
  else if (auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
    const bool lhs = collectExpressionRoots(binary->lhs.get(), roots);
    const bool rhs = collectExpressionRoots(binary->rhs.get(), roots);
    return lhs && rhs;
  } else if (auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
    const bool cond = collectExpressionRoots(ternary->cond.get(), roots);
    const bool yes = collectExpressionRoots(ternary->yes.get(), roots);
    const bool no = collectExpressionRoots(ternary->no.get(), roots);
    return cond && yes && no;
  } else if (auto* call = dynamic_cast<const CallExpr*>(expr)) {
    if (call->name != "ADDR" && call->name != "SIZEOF" && call->name != "LOADADDR" &&
        call->name != "ALIGNOF" && call->name != "ORIGIN" && call->name != "LENGTH")
      for (const auto& arg : call->args) collectExpressionRoots(arg.get(), roots);
  }
  return false;
}

bool applyScriptGC(LinkState& state, const script::ScriptFile& script, std::string& error) {
  using Action = script::SectionPattern::Action;
  std::vector<LtoSectionUse> uses(state.inputSections.size(), LtoSectionUse::Available);
  std::vector<bool> matched(uses.size());
  std::set<std::string> roots;
  for (const auto& region : script.memory) {
    collectExpressionRoots(region.origin.get(), roots);
    collectExpressionRoots(region.length.get(), roots);
  }
  for (const auto& statement : script.sectionsBody) {
    if (auto* assign = dynamic_cast<const script::AssignStmt*>(statement.get())) {
      if (assign->name != ".")
        throw std::runtime_error("--lto-script section GC does not support layout symbol assignments; use KEEP for implicit roots");
      collectExpressionRoots(assign->value.get(), roots);
    } else if (auto* assertion = dynamic_cast<const script::AssertStmt*>(statement.get())) {
      collectExpressionRoots(assertion->cond.get(), roots);
    } else if (auto* output = dynamic_cast<const script::OutputSectionStmt*>(statement.get())) {
      const auto& section = output->section;
      const bool discard = section.name == "/DISCARD/";
      collectExpressionRoots(section.address.get(), roots);
      collectExpressionRoots(section.alignExpr.get(), roots);
      collectExpressionRoots(section.fillExpr.get(), roots);
      for (const auto& pattern : section.inputs) {
        if (discard && (pattern.action != Action::Input || pattern.section == "COMMON" || pattern.keep))
          throw std::runtime_error("unsupported KEEP/directive/COMMON selector in /DISCARD/");
        if (pattern.action != Action::Input) {
          if (pattern.action == Action::Assign && pattern.symbol != ".")
            throw std::runtime_error("--lto-script section GC does not support layout symbol assignments; use KEEP for implicit roots");
          if (!collectExpressionRoots(pattern.expression.get(), roots) && pattern.action == Action::Data)
            throw std::runtime_error("--lto-script section GC supports only constant script data expressions");
          continue;
        }
        for (size_t i = 0; i < uses.size(); ++i) {
          if (!inputMatches(state, pattern, state.inputSections[i])) continue;
          if (!matched[i] && discard) uses[i] = LtoSectionUse::Discard;
          if (pattern.keep) {
            if (uses[i] == LtoSectionUse::Discard)
              throw std::runtime_error("KEEP conflicts with /DISCARD/ for " + state.inputSections[i].section.name);
            uses[i] = LtoSectionUse::Keep;
          }
          matched[i] = true;
        }
      }
    }
  }
  return collectLtoSections(state, uses, roots, error);
}

// GNU attributes select an unassigned section if any positive flag matches
// and no flag following '!' matches. They are selectors, not CPU permissions.
bool regionMatches(const std::string& attrs, uint32_t flags, bool noload) {
  if (attrs.empty()) return false;
  bool negative = false, positive = false, matched = false;
  for (unsigned char ch : attrs) {
    char c = char(std::tolower(ch));
    if (c == '!') { negative = true; continue; }
    bool hit = c == 'r' ? !(flags & qbfd::sec::Writable) :
               c == 'w' ? bool(flags & qbfd::sec::Writable) :
               c == 'x' ? bool(flags & (qbfd::sec::Code | qbfd::sec::Exec)) :
               c == 'a' ? bool(flags & qbfd::sec::Alloc) :
               (c == 'i' || c == 'l') ? !noload && !(flags & qbfd::sec::Bss) : false;
    if (negative && hit) return false;
    if (!negative) { positive = true; matched |= hit; }
  }
  return !positive || matched;
}

struct EvalCtx {
  uint64_t dot = 0;
  const std::map<std::string, uint64_t>* syms = nullptr;
  const std::map<std::string, std::pair<uint64_t, uint64_t>>* mem = nullptr;
  const std::map<std::string, std::pair<uint64_t, uint64_t>>* secs =
      nullptr;  // name -> (addr,size)
  const std::map<std::string, uint64_t>* alignments = nullptr;
  bool* failed = nullptr;
  std::string* error = nullptr;
};

uint64_t evalExpr(const script::Expr* e, EvalCtx& ctx);

uint64_t evalCall(const script::CallExpr* c, EvalCtx& ctx) {
  auto arg = [&](size_t i) -> uint64_t {
    if (i >= c->args.size()) {
      throw std::runtime_error("missing argument to " + c->name);
    }
    return evalExpr(c->args[i].get(), ctx);
  };
  const std::string& n = c->name;
  if (n == "ALIGN") {
    if (c->args.size() == 1) {
      uint64_t a = arg(0);
      return alignUp(ctx.dot, a ? a : 1);
    }
    if (c->args.size() == 2) {
      uint64_t v = arg(0), a = arg(1);
      return alignUp(v, a ? a : 1);
    }
    throw std::runtime_error("ALIGN expects one or two arguments");
  }
  // Named queries must not turn misspelled sections/regions into address zero.
  if (n == "ADDR" || n == "SIZEOF" || n == "LOADADDR" || n == "ORIGIN" || n == "LENGTH" || n == "DEFINED") {
    if (c->args.size() != 1) throw std::runtime_error(n + " expects one name");
    auto *symbol = dynamic_cast<const script::SymbolExpr *>(c->args[0].get());
    if (!symbol) throw std::runtime_error(n + " expects a name");
    if (n == "DEFINED") return ctx.syms && ctx.syms->count(symbol->name);
    bool memory = n == "ORIGIN" || n == "LENGTH";
    const auto *table = memory ? ctx.mem : ctx.secs;
    if (!table || !table->count(symbol->name)) throw std::runtime_error("unknown name in " + n + ": " + symbol->name);
    auto value = table->at(symbol->name);
    return n == "SIZEOF" || n == "LENGTH" ? value.second : value.first;
  }
  if (n == "ABSOLUTE") { if (c->args.size() != 1) throw std::runtime_error("ABSOLUTE expects one argument"); return arg(0); }
  if (n == "CONSTANT") {
    if (c->args.empty()) return 0;
    auto* se = dynamic_cast<const script::SymbolExpr*>(c->args[0].get());
    std::string k = se ? se->name : "";
    if (k == "MAXPAGESIZE") return 0x1000;
    if (k == "COMMONPAGESIZE") return 0x1000;
    return arg(0);
  }
  if (n == "SEGMENT_START") {
    if (c->args.size() != 2) throw std::runtime_error("SEGMENT_START expects name, default");
    return arg(1);
  }
  if (n == "MAX" || n == "MIN") {
    if (c->args.empty()) return 0;
    uint64_t v = arg(0);
    for (size_t i = 1; i < c->args.size(); ++i) {
      uint64_t w = arg(i);
      v = (n == "MAX") ? std::max(v, w) : std::min(v, w);
    }
    return v;
  }
  if (n == "DATA_SEGMENT_ALIGN" || n == "DATA_SEGMENT_RELRO_END" ||
      n == "DATA_SEGMENT_END" || n == "SEGMENT_START") {
    throw std::runtime_error("unsupported segment expression: " + n);
  }
  if (n == "ALIGNOF") {
    if (c->args.size() != 1 || !ctx.alignments) throw std::runtime_error("ALIGNOF expects a laid-out section");
    auto *s = dynamic_cast<const script::SymbolExpr *>(c->args[0].get());
    if (!s || !ctx.alignments->count(s->name)) throw std::runtime_error("unknown section in ALIGNOF");
    return ctx.alignments->at(s->name);
  }
  throw std::runtime_error("unknown linker expression function: " + n);
}

uint64_t evalExpr(const script::Expr* e, EvalCtx& ctx) {
  if (!e) return 0;
  if (auto* n = dynamic_cast<const script::NumberExpr*>(e)) return n->value;
  if (dynamic_cast<const script::DotExpr*>(e)) return ctx.dot;
  if (auto* s = dynamic_cast<const script::SymbolExpr*>(e)) {
    if (ctx.syms) {
      auto it = ctx.syms->find(s->name);
      if (it != ctx.syms->end()) return it->second;
    }
    throw std::runtime_error("undefined or forward-referenced script symbol: " + s->name);
  }
  if (auto* u = dynamic_cast<const script::UnaryExpr*>(e)) {
    uint64_t a = evalExpr(u->arg.get(), ctx);
    if (u->op == "-") return uint64_t(0) - a;
    if (u->op == "~") return ~a;
    if (u->op == "!") return a ? 0 : 1;
    return a;
  }
  if (auto* b = dynamic_cast<const script::BinaryExpr*>(e)) {
    uint64_t l = evalExpr(b->lhs.get(), ctx);
    if (b->op == "&&" && !l) return 0;
    if (b->op == "||" && l) return 1;
    uint64_t r = evalExpr(b->rhs.get(), ctx);
    const std::string& o = b->op;
    if (o == "+") return l + r;
    if (o == "-") return l - r;
    if (o == "*") return l * r;
    if ((o == "/" || o == "%") && !r) throw std::runtime_error("division by zero in linker expression");
    if (o == "/") return l / r;
    if (o == "%") return l % r;
    if ((o == "<<" || o == ">>") && r >= 64) throw std::runtime_error("invalid shift in linker expression");
    if (o == "<<") return l << r;
    if (o == ">>") return l >> r;
    if (o == "&") return l & r;
    if (o == "|") return l | r;
    if (o == "^") return l ^ r;
    if (o == "==") return l == r;
    if (o == "!=") return l != r;
    if (o == "<") return l < r;
    if (o == ">") return l > r;
    if (o == "<=") return l <= r;
    if (o == ">=") return l >= r;
    if (o == "&&") return (l && r) ? 1 : 0;
    if (o == "||") return (l || r) ? 1 : 0;
    return 0;
  }
  if (auto* t = dynamic_cast<const script::TernaryExpr*>(e)) {
    return evalExpr(t->cond.get(), ctx) ? evalExpr(t->yes.get(), ctx)
                                        : evalExpr(t->no.get(), ctx);
  }
  if (auto* c = dynamic_cast<const script::CallExpr*>(e))
    return evalCall(c, ctx);
  return 0;
}

bool loadScriptFile(const std::string& path, std::string& text,
                    std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open script " + path;
    return false;
  }
  text.assign((std::istreambuf_iterator<char>(in)),
              std::istreambuf_iterator<char>());
  return true;
}

std::string readScriptSource(const LinkState& state, std::string& error) {
  if (state.options.scriptIsText) return state.options.scriptText;
  if (!state.options.script.empty()) {
    std::string text;
    if (!loadScriptFile(state.options.script, text, error)) return {};
    if (!error.empty()) return {};
    return text;
  }
  // Scripts passed as INPUT files are already in objects[] with isScript.
  std::string combined;
  for (const auto& o : state.objects) {
    if (!o.isScript) continue;
    combined += std::string(o.bytes.begin(), o.bytes.end());
    combined += "\n";
  }
  return combined;
}

}  // namespace

bool hasScript(const LinkState& state) {
  if (state.scriptExtrasOnly) return false;
  if (state.options.scriptIsText && !state.options.scriptText.empty())
    return true;
  if (!state.options.script.empty()) return true;
  for (const auto& o : state.objects)
    if (o.isScript) return true;
  return false;
}

// Loads GROUP/INPUT/SEARCH_DIR extras. Called by the driver before discovery.
bool loadScriptExtras(LinkState& state, std::string& error) {
  std::string src = readScriptSource(state, error);
  if (!error.empty()) return false;
  if (src.empty()) return true;
  try {
    src = qld::ScriptSDK{}.preprocess(src, state.options.scriptIsText ? "<script>" : state.options.script);
  } catch (const std::exception &e) { error = e.what(); return false; }
  // Lua and extensions execute once, before discovery; layout reuses the result.
  state.options.scriptText = src;
  state.options.scriptIsText = true;
  script::ScriptFile sf;
  if (!script::parseText(src, sf, error)) return false;
  state.scriptExtrasOnly = !sf.hasSections;
  state.scriptPrepared = true;
  for (const auto &statement : sf.sectionsBody) {
    if (const auto *a = dynamic_cast<const script::AssignStmt *>(statement.get())) {
      if (a->name != ".") state.scriptSymbols.insert(a->name);
    } else if (const auto *section = dynamic_cast<const script::OutputSectionStmt *>(statement.get())) {
      for (const auto &item : section->section.inputs)
        if (item.action == script::SectionPattern::Action::Assign && item.symbol != ".") state.scriptSymbols.insert(item.symbol);
    }
  }
  for (const auto& d : sf.searchDirs) state.options.libPaths.push_back(d);
  if (sf.entry && !state.options.hasEntry) {
    // An explicit -e wins, even when its value is the default _start.
    state.options.entry = *sf.entry;
  }
  // GROUP files join the input queue.
  for (const auto& g : sf.group) {
    std::string name = g;
    // Handle -lfoo inside scripts.
    if (name.starts_with("-l")) {
      state.options.libs.push_back(name.substr(2));
      continue;
    }
    // AS_NEEDED(...) already unwrapped by the parser.
    if (!state.options.script.empty() && !std::filesystem::path(name).is_absolute()) {
      auto relative = std::filesystem::path(state.options.script).parent_path() / name;
      if (std::filesystem::is_regular_file(relative)) name = relative.string();
    }
    std::ifstream probe(name, std::ios::binary);
    std::string resolved = name;
    if (!probe) {
      // Search libPaths.
      bool found = false;
      for (const auto& dir : state.options.libPaths) {
        std::string cand = (std::filesystem::path(dir) / name).string();
        std::ifstream p2(cand, std::ios::binary);
        if (p2) {
          resolved = cand;
          found = true;
          break;
        }
      }
      if (!found) {
        error = "script: cannot find input " + name;
        return false;
      }
    }
    // Avoid double-adding command-line inputs.
    bool dup = false;
    for (const auto& o : state.objects)
      if (o.label == resolved) {
        dup = true;
        break;
      }
    if (dup) continue;
    std::ifstream in(resolved, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (qbfd::isArchive(bytes)) {
      auto opened = qbfd::openArchive(resolved);
      if (!opened) {
        error = resolved + ": " + opened.error().message;
        return false;
      }
      LoadedArchive la;
      la.path = resolved;
      auto buf = qbfd::Buffer::fromFile(resolved);
      if (buf) {
        la.buffer = std::move(*buf);
        la.bytes.assign(la.buffer->span().begin(), la.buffer->span().end());
      } else {
        la.bytes = std::move(bytes);
      }
      la.archive = std::move(opened->archive);
      state.archives.push_back(std::move(la));
    } else {
      const bool lto = !state.options.ltoScript.empty();
      if (lto && !validateLtoInput(bytes, resolved, error)) return false;
      auto opened = qbfd::open(bytes, resolved);
      if (!opened) {
        error = resolved + ": INPUT expects an ELF object/archive; use INCLUDE for nested scripts";
        return false;
      }
      if (lto && !validateLtoObject(*opened->object, resolved, error)) return false;
      LoadedObject lo;
      lo.label = resolved;
      lo.bytes = std::move(bytes);
      lo.buffer = std::move(opened->buffer);
      lo.object = std::move(opened->object);
      state.objects.push_back(std::move(lo));
    }
  }
  return true;
}

bool runScript(LinkState& state, std::string& error) try {
  std::string src = readScriptSource(state, error);
  if (!error.empty()) return false;
  if (src.empty()) {
    error = "internal error: script requested but no source found";
    return false;
  }
  script::ScriptFile sf;
  if (!script::parseText(src, sf, error)) return false;
  if (sf.entry && !state.options.hasEntry) state.options.entry = *sf.entry;

  if (!collectInputSections(state, true, error)) return false;
  if (state.ltoPlan.gcSections && !applyScriptGC(state, sf, error)) return false;

  // MEMORY regions.
  std::map<std::string, std::pair<uint64_t, uint64_t>> mem;  // name->(org,len)
  std::map<std::string, uint64_t> memCursor;
  std::map<std::string, std::string> aliases;
  auto regionName = [&](const std::string& name) -> std::string {
    auto alias = aliases.find(name);
    std::string canonical = alias == aliases.end() ? name : alias->second;
    if (!mem.count(canonical)) throw std::runtime_error("unknown MEMORY region: " + name);
    return canonical;
  };
  {
    EvalCtx ctx;
    ctx.syms = &state.symAddr;
    ctx.mem = &mem;
    for (const auto& m : sf.memory) {
      uint64_t org = m.origin ? evalExpr(m.origin.get(), ctx) : 0;
      uint64_t len = m.length ? evalExpr(m.length.get(), ctx) : 0;
      if (!m.origin || !m.length || mem.count(m.name) || len > UINT64_MAX - org)
        throw std::runtime_error("invalid or duplicate MEMORY region: " + m.name);
      mem[m.name] = {org, len};
      memCursor[m.name] = org;
    }
    for (const auto& [alias, target] : sf.regionAliases) {
      if (alias.empty() || mem.count(alias) || aliases.count(alias))
        throw std::runtime_error("duplicate MEMORY region/alias: " + alias);
      aliases[alias] = regionName(target);
      mem[alias] = mem.at(aliases.at(alias)); // ORIGIN/LENGTH aliases, same placement cursor.
    }
  }
  auto automaticRegion = [&](uint32_t flags, bool noload) -> std::string {
    if (!(flags & qbfd::sec::Alloc)) return {};
    for (const auto& m : sf.memory)
      if (regionMatches(m.attrs, flags, noload)) return m.name;
    return {};
  };
  auto commitRegion = [&](const OutputSection& out) {
    if (out.region.empty()) return;
    const auto [origin, length] = mem.at(out.region);
    if (out.vma < origin || out.size > length || out.vma - origin > length - out.size)
      throw std::runtime_error("MEMORY region overflow: " + out.region);
    // Explicit addresses must not rewind placement of later sections.
    memCursor[out.region] = std::max(memCursor[out.region], out.vma + out.size);
  };

  bool relocatable = state.options.mode == Mode::Relocatable;
  uint64_t base = state.options.hasBase
                      ? state.options.base
                      : (state.options.mode == Mode::Exec && !state.options.pie
                             ? 0x400000
                             : 0);
  uint64_t ehsize = state.wide ? 64 : 52;
  uint64_t phentsize = state.wide ? 56 : 32;
  uint64_t phnum = relocatable ? 0 : 3;
  uint64_t headerEnd = relocatable ? ehsize : ehsize + phnum * phentsize;
  uint64_t dot = (base == 0) ? headerEnd : alignUp(base + headerEnd, 0x1000);

  for (const auto& [n, d] : state.defs)
    if (d.symbol.flags & qbfd::sym::Absolute) state.symAddr[n] = d.symbol.value;

  std::map<std::string, std::pair<uint64_t, uint64_t>> secAddr;  // for ADDR()
  std::map<std::string, uint64_t> secAlignment;
  state.outSections.clear();
  std::vector<bool> consumed(state.inputSections.size(), false);

  auto takeMatches = [&](const script::SectionPattern& pat) {
    std::vector<size_t> hits;
    for (size_t i = 0; i < state.inputSections.size(); ++i) {
      if (consumed[i]) continue;
      const auto& in = state.inputSections[i];
      if (!in.kept || !inputMatches(state, pat, in)) continue;
      hits.push_back(i);
    }
    if (pat.sort == "SORT" || pat.sort == "SORT_BY_NAME")
      std::stable_sort(hits.begin(), hits.end(), [&](size_t a, size_t b) { return state.inputSections[a].section.name < state.inputSections[b].section.name; });
    else if (pat.sort == "SORT_BY_ALIGNMENT")
      std::stable_sort(hits.begin(), hits.end(), [&](size_t a, size_t b) { return state.inputSections[a].section.alignment > state.inputSections[b].section.alignment; });
    return hits;
  };

  // No SECTIONS: only MEMORY/ENTRY mattered; fall back to default order.
  if (!sf.hasSections) {
    // Reuse default layout by clearing script state and delegating. To
    // avoid recursion, inline a minimal default here.
    error = "script has no SECTIONS command";
    return false;
  }

  EvalCtx ctx;
  ctx.syms = &state.symAddr;
  ctx.mem = &mem;
  ctx.secs = &secAddr;
  ctx.alignments = &secAlignment;
  ctx.dot = dot;

  uint32_t nextIndex = 1;
  std::set<std::string> placedCommons;
  for (const auto& st : sf.sectionsBody) {
    if (auto* a = dynamic_cast<script::AssignStmt*>(st.get())) {
      ctx.dot = dot;
      if (a->provide && state.defs.count(a->name)) continue;
      uint64_t v = evalExpr(a->value.get(), ctx);
      if (a->name == ".") {
        if (a->op == "=") dot = v;
        else if (a->op == "+=") dot += v;
        else if (a->op == "-=") dot -= v;
        else if (a->op == "*=") dot *= v;
        else if (a->op == "/=") dot = v ? dot / v : dot;
        else if (a->op == "&=") dot &= v;
        else if (a->op == "|=") dot |= v;
      } else {
        bool exists = state.symAddr.count(a->name) != 0;
        if (a->provide && exists) continue;  // PROVIDE: only when undefined
        uint64_t cur = exists ? state.symAddr[a->name] : 0;
        uint64_t nv = cur;
        if (a->op == "=") nv = v;
        else if (a->op == "+=") nv = cur + v;
        else if (a->op == "-=") nv = cur - v;
        else if (a->op == "*=") nv = cur * v;
        else if (a->op == "/=") nv = v ? cur / v : cur;
        else if (a->op == "&=") nv = cur & v;
        else if (a->op == "|=") nv = cur | v;
        state.symAddr[a->name] = nv;
        // A script assignment also defines the symbol for the final table
        // when no object defined it.
        if (!state.defs.count(a->name)) {
          Def d;
          d.objectIndex = 0;
          d.symbolIndex = 0;
          d.symbol.name = a->name;
          d.symbol.value = nv;
          d.symbol.binding = qbfd::SymbolBinding::Global;
          d.symbol.flags = qbfd::sym::Absolute;
          state.defs.emplace(a->name, std::move(d));
        } else {
          // Update absolute script symbols to the assigned value.
          auto& dd = state.defs[a->name];
          if (dd.symbol.flags & qbfd::sym::Absolute) dd.symbol.value = nv;
        }
      }
      ctx.dot = dot;
      continue;
    }
    if (auto* as = dynamic_cast<script::AssertStmt*>(st.get())) {
      ctx.dot = dot;
      if (!evalExpr(as->cond.get(), ctx)) {
        error = "ASSERT failed" +
                (as->message.empty() ? "" : std::string(": ") + as->message);
        return false;
      }
      continue;
    }
    auto* os = dynamic_cast<script::OutputSectionStmt*>(st.get());
    if (!os) continue;
    const auto& sec = os->section;
    if (!sec.type.empty() && sec.type != "NOLOAD") throw std::runtime_error("unsupported output section type: " + sec.type);
    if (sec.lma || !sec.lmaRegion.empty()) throw std::runtime_error("AT/LMA layouts are unsupported by the ELF segment writer");
    if (sec.name == "/DISCARD/") {
      for (const auto& pat : sec.inputs) {
        if (pat.action != script::SectionPattern::Action::Input || pat.section == "COMMON")
          throw std::runtime_error("unsupported directive/COMMON selector in /DISCARD/");
        for (size_t hit : takeMatches(pat)) {
          consumed[hit] = true;
          state.inputSections[hit].kept = false;
        }
      }
      continue;
    }
    uint32_t inputFlags = 0;
    bool commonInput = false;
    for (const auto& pat : sec.inputs) if (pat.action == script::SectionPattern::Action::Input) {
      for (auto hit : takeMatches(pat)) inputFlags |= state.inputSections[hit].section.flags;
      if (pat.section == "COMMON") {
        if (!pat.file.empty() && pat.file != "*")
          throw std::runtime_error("file-selective COMMON allocation is unsupported; use *(COMMON)");
        for (const auto& [name, d] : state.defs) if (d.common && !placedCommons.count(name)) commonInput = true;
      }
    }
    if (commonInput) inputFlags |= qbfd::sec::Alloc | qbfd::sec::Writable | qbfd::sec::Bss;
    // Script-created bytes are allocated even without input contributions.
    if (!inputFlags) {
      inputFlags = qbfd::sec::Alloc;
      if (sec.name.starts_with(".data") || sec.name.starts_with(".bss")) inputFlags |= qbfd::sec::Writable;
      if (sec.name.starts_with(".bss")) inputFlags |= qbfd::sec::Bss;
      if (sec.name.starts_with(".text")) inputFlags |= qbfd::sec::Code | qbfd::sec::Exec;
    }
    std::string region = sec.region.empty() ?
        (sec.address ? "" : automaticRegion(inputFlags, sec.type == "NOLOAD")) : regionName(sec.region);
    // Address: explicit expression, region cursor, or current dot.
    uint64_t vma = dot;
    ctx.dot = dot;
    if (sec.address) vma = evalExpr(sec.address.get(), ctx);
    if (!region.empty()) {
      auto it = memCursor.find(region);
      if (it == memCursor.end()) {
        error = "unknown MEMORY region: " + region;
        return false;
      }
      if (!sec.address) vma = it->second;
    }
    if (sec.alignExpr) {
      uint64_t a = evalExpr(sec.alignExpr.get(), ctx);
      vma = alignUp(vma, a ? a : 1);
    }
    uint64_t requiredAlignment = 1;
    for (const auto &pat : sec.inputs) if (pat.action == script::SectionPattern::Action::Input)
      for (auto hit : takeMatches(pat)) requiredAlignment = std::max(requiredAlignment, state.inputSections[hit].section.alignment);
    if (commonInput) for (const auto& [name, d] : state.defs)
      if (d.common && !placedCommons.count(name)) requiredAlignment = std::max(requiredAlignment, d.commonAlign);
    if (sec.alignExpr) {
      uint64_t alignment = evalExpr(sec.alignExpr.get(), ctx);
      if (!alignment || (alignment & (alignment - 1))) throw std::runtime_error("section ALIGN must be a power of two");
      requiredAlignment = std::max(requiredAlignment, alignment);
    }
    if (!sec.address) vma = alignUp(vma, requiredAlignment);
    else if (vma % requiredAlignment) throw std::runtime_error("output section address violates input alignment: " + sec.name);
    // Gather members.
    OutputSection out;
    out.name = sec.name;
    out.region = region;
    out.index = nextIndex++;
    out.alignment = requiredAlignment;
    // Type NOLOAD: BSS-like, occupies VMA but no file bytes.
    bool noload = (sec.type == "NOLOAD");
    uint64_t off = 0;
    bool scriptBytes = false;
    uint8_t fill = 0;
    if (sec.fillExpr) {
      auto value = evalExpr(sec.fillExpr.get(), ctx);
      if (value > 255) throw std::runtime_error("section fill currently requires one byte");
      fill = static_cast<uint8_t>(value);
    }
    auto grow = [&](uint64_t size) {
      if (size > 256 * 1024 * 1024) throw std::runtime_error("script output section exceeds 256 MiB");
      if (!noload) out.scriptData.resize(static_cast<size_t>(size), fill);
    };
    for (const auto& pat : sec.inputs) {
      using Action = script::SectionPattern::Action;
      if (pat.action != Action::Input) {
        ctx.dot = vma + off;
        if (pat.provide && state.defs.count(pat.symbol)) continue;
        uint64_t value = evalExpr(pat.expression.get(), ctx);
        if (pat.action == Action::Fill) {
          if (value > 255) throw std::runtime_error("FILL currently requires one byte");
          fill = static_cast<uint8_t>(value);
        } else if (pat.action == Action::Assert) {
          if (!value) throw std::runtime_error("ASSERT failed: " + pat.message);
        } else if (pat.action == Action::Data) {
          scriptBytes = true;
          if (noload) throw std::runtime_error("data directive in NOLOAD section");
          if (pat.width < 8 && value >= (UINT64_C(1) << (pat.width * 8))) throw std::runtime_error("script data value out of range");
          grow(off + pat.width);
          for (unsigned b = 0; b < pat.width; ++b) {
            unsigned shift = state.endian == qbfd::Endian::Little ? b : pat.width - b - 1;
            out.scriptData[off + b] = static_cast<uint8_t>(value >> (shift * 8));
          }
          off += pat.width;
        } else if (pat.symbol == ".") {
          uint64_t current = vma + off;
          if (pat.operation == "+=") value += current;
          else if (pat.operation == "-=") value = current - value;
          else if (pat.operation != "=") throw std::runtime_error("unsupported location-counter assignment: " + pat.operation);
          if (value < current || value < vma) throw std::runtime_error("location counter moves backwards or overflows");
          off = value - vma; grow(off);
        } else {
          state.symAddr[pat.symbol] = value;
          auto &d = state.defs[pat.symbol]; d.symbol.name = pat.symbol;
          d.symbol.binding = qbfd::SymbolBinding::Global;
          d.symbol.flags = qbfd::sym::Absolute; d.symbol.value = value;
        }
        continue;
      }
      if (pat.section == "COMMON") {
        for (auto& [name, d] : state.defs) {
          if (!d.common || placedCommons.count(name)) continue;
          off = alignUp(off, std::max<uint64_t>(d.commonAlign, 1));
          if (d.commonSize > UINT64_MAX - off || off > UINT64_MAX - vma)
            throw std::runtime_error("COMMON section overflow");
          state.symAddr[name] = vma + off;
          d.commonOutputSection = out.index;
          off += d.commonSize;
          placedCommons.insert(name);
          out.flags |= qbfd::sec::Alloc | qbfd::sec::Writable | qbfd::sec::Bss;
        }
      }
      for (size_t hit : takeMatches(pat)) {
        consumed[hit] = true;
        auto& in = state.inputSections[hit];
        if (state.ltoPlan.gcSections && noload && !in.contents.empty())
          throw std::runtime_error("--lto-script section GC cannot place initialized input in NOLOAD: " + in.section.name);
        uint64_t a = in.section.alignment ? in.section.alignment : 1;
        out.alignment = std::max(out.alignment, a);
        out.flags |= in.section.flags;
        off = alignUp(off, a);
        in.outOffset = off;
        uint64_t size = std::max<uint64_t>(in.section.size, in.contents.size());
        if (size > UINT64_MAX - off) throw std::runtime_error("section size overflow");
        off += size;
        grow(off);
        out.inputs.push_back(hit);
      }
    }
    out.size = off;
    bool anyFile = false;
    for (size_t idx : out.inputs) {
      if (!(state.inputSections[idx].section.flags & qbfd::sec::Bss)) {
        anyFile = true;
        break;
      }
    }
    out.bss = noload || (!scriptBytes && (!out.inputs.empty() || commonInput) && !anyFile);
    // Empty script sections (no inputs) still exist with size 0 at dot.
    out.vma = vma;
    out.alloc = true;  // script sections are alloc by default
    // Heuristic flags when empty.
    if (out.inputs.empty()) {
      if (out.name == ".bss" || out.name.starts_with(".bss"))
        out.flags |= qbfd::sec::Bss | qbfd::sec::Alloc | qbfd::sec::Writable;
      else if (out.name == ".text" || out.name.starts_with(".text"))
        out.flags |= qbfd::sec::Alloc | qbfd::sec::Code | qbfd::sec::Exec;
      else if (out.name == ".data" || out.name.starts_with(".data"))
        out.flags |=
            qbfd::sec::Alloc | qbfd::sec::Data | qbfd::sec::Writable;
      else
        out.flags |= qbfd::sec::Alloc;
      out.alloc = true;
    }
    out.executable = (out.flags & (qbfd::sec::Code | qbfd::sec::Exec)) != 0;
    out.writable = (out.flags & qbfd::sec::Writable) != 0 ||
                   out.name == ".data" || out.name == ".bss";
    // File offsets: BSS/NOLOAD occupy no file space; others follow dot
    // linearly like the default layout.
    out.fileSize = out.bss ? 0 : out.size;
    secAddr[out.name] = {out.vma, out.size};
    secAlignment[out.name] = out.alignment;
    if (out.size > UINT64_MAX - vma) throw std::runtime_error("section address overflow");
    dot = vma + out.size;
    commitRegion(out);
    for (const auto &[name, d] : state.defs) if (d.symbol.section) {
      for (size_t idx : out.inputs) {
        const auto &in = state.inputSections[idx];
        if (in.objectIndex == d.objectIndex && in.sectionIndex == *d.symbol.section)
          state.symAddr[name] = vma + in.outOffset + d.symbol.value;
      }
    }
    state.outSections.push_back(std::move(out));
  }

  // Orphan sections (not consumed by the script) go after, like GNU ld.
  {
    std::map<std::string, std::vector<size_t>> orphans;
    std::vector<std::string> oorder;
    for (size_t i = 0; i < state.inputSections.size(); ++i) {
      if (consumed[i]) continue;
      auto& in = state.inputSections[i];
      if (!in.kept) continue;
      std::string name = in.section.name.empty() ? ".orphan" : in.section.name;
      if (!orphans.count(name)) oorder.push_back(name);
      orphans[name].push_back(i);
    }
    for (const auto& name : oorder) {
      OutputSection out;
      out.name = name;
      out.index = nextIndex++;
      out.alignment = 1;
      for (size_t idx : orphans[name]) {
        auto& in = state.inputSections[idx];
        out.alignment =
            std::max(out.alignment, in.section.alignment
                                        ? in.section.alignment
                                        : uint64_t(1));
        out.flags |= in.section.flags;
        out.inputs.push_back(idx);
      }
      uint64_t off = 0;
      for (size_t idx : out.inputs) {
        auto& in = state.inputSections[idx];
        off = alignUp(off, in.section.alignment ? in.section.alignment : 1);
        in.outOffset = off;
        off += std::max<uint64_t>(in.section.size, in.contents.size());
      }
      out.size = off;
      bool anyFile = false;
      for (size_t idx : out.inputs) {
        if (!(state.inputSections[idx].section.flags & qbfd::sec::Bss)) {
          anyFile = true;
          break;
        }
      }
      out.bss = !anyFile && !out.inputs.empty();
      out.fileSize = out.bss ? 0 : out.size;
      out.alloc = (out.flags & qbfd::sec::Alloc) != 0;
      if (out.inputs.empty()) out.alloc = false;
      else if (name.starts_with(".debug") || name == ".comment")
        out.alloc = false;
      out.executable = (out.flags & (qbfd::sec::Code | qbfd::sec::Exec)) != 0;
      out.writable = (out.flags & qbfd::sec::Writable) != 0;
      out.region = automaticRegion(out.flags, out.bss);
      out.vma = alignUp(out.region.empty() ? dot : memCursor.at(out.region), out.alignment);
      if (out.size > UINT64_MAX - out.vma) throw std::runtime_error("orphan section address overflow");
      if (out.alloc) dot = out.vma + out.size;
      commitRegion(out);
      secAddr[out.name] = {out.vma, out.size};
      state.outSections.push_back(std::move(out));
    }
  }

  // File offsets for alloc sections: VMA - base (first LOAD) style.
  // For a fixed base the file offset must stay congruent to the VMA modulo
  // the page size, otherwise the kernel maps the entry to the wrong bytes.
  // Setting fileOffset = VMA - base for every file-backed section keeps
  // that congruence exactly (gaps in VMA and file stay identical).
  {
    uint64_t f = headerEnd;
    if (base != 0) {
      for (auto& o : state.outSections) {
        if (!o.alloc) continue;
        if (o.vma < base || (!o.bss && o.fileSize && o.vma - base < headerEnd))
          throw std::runtime_error("script section precedes image base or overlaps ELF headers: " + o.name);
        if (o.bss) {
          o.fileOffset = o.vma - base;
          continue;
        }
        o.fileOffset = o.vma - base;
      }
      // Non-alloc sections follow the last alloc file end.
      uint64_t end = headerEnd;
      for (const auto& o : state.outSections) {
        if (!o.alloc || o.bss) continue;
        end = std::max(end, o.fileOffset + o.fileSize);
      }
      for (auto& o : state.outSections) {
        if (o.alloc) continue;
        end = alignUp(end, std::max<uint64_t>(o.alignment, 1));
        o.fileOffset = end;
        end += o.fileSize;
      }
    } else {
      for (auto& o : state.outSections) {
        if (!o.alloc) continue;
        if (o.bss) {
          o.fileOffset = f;
          continue;
        }
        f = alignUp(f, std::max<uint64_t>(o.alignment, 1));
        o.fileOffset = f;
        f += o.fileSize;
      }
      // Non-alloc follow.
      for (auto& o : state.outSections) {
        if (o.alloc) continue;
        f = alignUp(f, std::max<uint64_t>(o.alignment, 1));
        o.fileOffset = f;
        f += o.fileSize;
      }
    }
  }

  // Symbol addresses (same rules as the default layout).
  for (const auto& [name, d] : state.defs) {
    if (d.common) continue;
    if (state.symAddr.count(name) &&
        (d.symbol.flags & qbfd::sym::Absolute))
      continue;  // script-assigned absolute already set
    const auto& sym = d.symbol;
    if (sym.flags & qbfd::sym::Absolute) {
      if (!state.symAddr.count(name)) state.symAddr[name] = sym.value;
      continue;
    }
    if (!sym.section) {
      if (!state.symAddr.count(name) || state.symAddr[name] == 0)
        state.symAddr[name] = sym.value;
      continue;
    }
    bool found = false;
    for (const auto& o : state.outSections) {
      for (size_t idx : o.inputs) {
        const auto& in = state.inputSections[idx];
        if (in.objectIndex == d.objectIndex &&
            in.sectionIndex == *sym.section) {
          state.symAddr[name] = o.vma + in.outOffset + sym.value;
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (!found && !state.symAddr.count(name)) state.symAddr[name] = sym.value;
  }
  // COMMONs to .bss tail (create .bss if missing).
  {
    bool need = false;
    for (const auto& [n, d] : state.defs)
      if (d.common && !placedCommons.count(n)) need = true;
    if (need && !secAddr.count(".bss")) {
      OutputSection b;
      b.name = ".bss";
      b.index = nextIndex++;
      b.flags =
          qbfd::sec::Alloc | qbfd::sec::Writable | qbfd::sec::Bss |
          qbfd::sec::Data;
      b.alignment = 8;
      b.alloc = true;
      b.bss = true;
      for (const auto& [name, d] : state.defs)
        if (d.common && !placedCommons.count(name)) b.alignment = std::max(b.alignment, d.commonAlign);
      b.region = automaticRegion(b.flags, true);
      b.vma = alignUp(b.region.empty() ? dot : memCursor.at(b.region), b.alignment);
      b.fileSize = 0;
      b.size = 0;
      state.outSections.push_back(std::move(b));
      secAddr[".bss"] = {dot, 0};
    }
    for (auto& o : state.outSections) {
      if (o.name != ".bss") continue;
      uint64_t tail = o.size;
      for (auto& [name, d] : state.defs) {
        if (!d.common || placedCommons.count(name)) continue;
        tail = alignUp(tail, d.commonAlign ? d.commonAlign : 1);
        if (tail > UINT64_MAX - o.vma || d.commonSize > UINT64_MAX - tail)
          throw std::runtime_error("COMMON section overflow");
        state.symAddr[name] = o.vma + tail;
        d.commonOutputSection = o.index;
        tail += d.commonSize;
        placedCommons.insert(name);
      }
      o.size = tail;
      commitRegion(o);
      secAddr[".bss"] = {o.vma, o.size};
      break;
    }
  }

  for (size_t i = 0; i < state.outSections.size(); ++i) {
    const auto &a = state.outSections[i];
    if (a.fileOffset > 256 * 1024 * 1024 || a.fileSize > 256 * 1024 * 1024 - a.fileOffset)
      throw std::runtime_error("script file image exceeds 256 MiB");
    if (!a.alloc || !a.size) continue;
    for (size_t j = 0; j < i; ++j) {
      const auto &b = state.outSections[j];
      if (b.alloc && b.size && a.vma < b.vma + b.size && b.vma < a.vma + a.size)
        throw std::runtime_error("overlapping output sections: " + b.name + " and " + a.name);
    }
  }
  // Entry.
  {
    auto it = state.symAddr.find(state.options.entry);
    state.entryAddr = it == state.symAddr.end() ? 0 : it->second;
  }
  if (!state.options.mapFile.empty()) {
    std::ostringstream map;
    map << "Memory Configuration\n\nName Origin Length\n" << std::hex << std::showbase;
    for (const auto& m : sf.memory)
      map << m.name << ' ' << mem.at(m.name).first << ' ' << mem.at(m.name).second << ' ' << m.attrs << '\n';
    for (const auto& [alias, target] : aliases) map << alias << " -> " << target << '\n';
    map << "\nSection Mapping\n\nName Address Size FileOffset Region\n";
    for (const auto& o : state.outSections) {
      map << o.name << ' ' << o.vma << ' ' << o.size << ' ' << o.fileOffset << ' ' << o.region << '\n';
      for (auto idx : o.inputs) {
        const auto& in = state.inputSections[idx];
        map << "  " << state.objects[in.objectIndex].label << '(' << in.section.name << ") "
            << o.vma + in.outOffset << ' ' << in.section.size << '\n';
      }
    }
    map << "\nSymbols\n";
    for (const auto& [name, address] : state.symAddr) map << name << ' ' << address << '\n';
    state.mapText = map.str();
  }
  return true;
} catch (const std::exception &e) {
  error = std::string("linker script: ") + e.what();
  return false;
}

}  // namespace qld::detail
