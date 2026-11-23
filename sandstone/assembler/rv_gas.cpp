// RV GAS syntax adapter. Produces bank mnemonics, never instruction bytes.
#include "intern.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using std::string;
using Args = std::vector<string>;
string trim(const string &s) {
  auto i = s.find_first_not_of(" \t\r");
  return i == s.npos ? "" : s.substr(i, s.find_last_not_of(" \t\r") - i + 1);
}
Args split(const string &s) {
  Args args;
  for (size_t i = 0; i < s.size();) {
    auto j = s.find(',', i);
    args.push_back(trim(s.substr(i, j - i)));
    if (j == s.npos)
      break;
    i = j + 1;
    if (i == s.size())
      throw std::runtime_error("trailing operand separator");
  }
  return args;
}
void check(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
struct Options {
  bool pic = false;
  std::vector<uint32_t> features;
};
struct Source {
  sandstone_asm_t *ctx;
  Options opt;
  std::vector<Options> stack;
  std::array<unsigned, 10> labels{}, forward{};
  unsigned anchor = 0;
  string prefix = ".LsandRV_", out;
  std::map<string, string> aliases;
  std::set<string> definitions;
  explicit Source(sandstone_asm_t *c, const string &source) : ctx(c) {
    while (source.find(prefix) != source.npos)
      prefix += "_";
    for (size_t i = 0; i < c->n_enabled; ++i)
      opt.features.push_back(c->enabled[i]);
    // Cephyr appends its TU-private call aliases after the referring
    // functions. Defined targets receive coincident labels; references
    // to undefined targets must carry the external symbol in RELA.
    std::istringstream in(source);
    string raw;
    while (std::getline(in, raw)) {
      auto s = trim(raw);
      auto pos = s.find(':');
      if (pos != s.npos && sand_is_name(s.substr(0, pos).c_str()))
        definitions.insert(s.substr(0, pos));
      if (s.starts_with(".set ") || s.starts_with(".set\t")) {
        auto a = split(trim(s.substr(4)));
        if (a.size() == 2 && a[0].starts_with("__cephyr_tu_") &&
            sand_is_name(a[0].c_str()) && sand_is_name(a[1].c_str()))
          check(aliases.emplace(a[0], a[1]).second,
                "duplicate TU-private alias");
      }
    }
    for (const auto &[alias, target] : aliases)
      check(!definitions.contains(alias) && !aliases.contains(target),
            "ambiguous TU-private alias");
  }
  string local(unsigned d, unsigned id) const {
    return prefix + std::to_string(d) + "_" + std::to_string(id);
  }
  string symbol(string s) {
    if (s.size() == 2 && s[0] >= '0' && s[0] <= '9' &&
        (s[1] == 'f' || s[1] == 'b')) {
      unsigned d = s[0] - '0', id = labels[d] + (s[1] == 'f');
      check(id != 0, "undefined backward numeric label");
      if (s[1] == 'f')
        forward[d] = id;
      return local(d, id);
    }
    check(sand_is_name(s.c_str()), "expected symbol name");
    if (auto i = aliases.find(s);
        i != aliases.end() && !definitions.contains(i->second))
      return i->second;
    return s;
  }
  // Validate against the scoped feature set and operand descriptors before
  // handing the normalized line to the ordinary two-pass assembler.
  void inst(const string &m, const Args &args) {
    uint32_t ids[64], count = 0;
    check(domsymas_resolve(ctx->bank, m.c_str(), args.size(),
                           opt.features.data(), opt.features.size(), ids, 64,
                           &count) == DOMSYMAS_OK &&
              count,
          "instruction unavailable in active ISA");
    bool match = false;
    for (uint32_t i = 0; i < count && !match; ++i) {
      bool valid = true;
      for (size_t j = 0; j < args.size() && valid; ++j) {
        auto d = domsymas_operand_at(ctx->bank, ids[i], j);
        auto r = domsymas_find_register(ctx->bank, args[j].c_str());
        int64_t imm;
        valid =
            d && (d->kind == 0   ? (r && r->regclass == d->regclass)
                  : d->kind == 1 ? (sand_parse_integer(args[j].c_str(), &imm) &&
                                    imm >= d->imm_min && imm <= d->imm_max)
                  : d->kind == 6 ? bool(sand_is_name(args[j].c_str()))
                                 : true);
      }
      match = valid;
    }
    check(match, "invalid operand class or immediate range");
    out += m;
    for (size_t i = 0; i < args.size(); ++i)
      out += (i ? ", " : " ") + args[i];
    out += '\n';
  }
  void option(const Args &a) {
    if (a == Args{"push"})
      stack.push_back(opt);
    else if (a == Args{"pop"}) {
      check(!stack.empty(), ".option pop without push");
      opt = stack.back();
      stack.pop_back();
    } else if (a == Args{"pic"})
      opt.pic = true;
    else if (a == Args{"nopic"})
      opt.pic = false;
    else if (a == Args{"norelax"}) {
    } // Bank never emits optional relax records.
    else if (a == Args{"norvc"}) {
      std::erase_if(opt.features, [&](auto id) {
        return string(domsymas_feature_at(ctx->bank, id)->name) == "c";
      });
    } else if (a == Args{"arch", "rv64i"} || a == Args{"arch", "rv64im"}) {
      opt.features.clear();
      for (size_t i = 0; i < ctx->n_enabled; ++i) {
        auto id = ctx->enabled[i];
        string f = domsymas_feature_at(ctx->bank, id)->name;
        if (f == "i" || (f == "m" && a[1] == "rv64im"))
          opt.features.push_back(id);
      }
      check(opt.features.size() == (a[1] == "rv64im" ? 2u : 1u),
            "requested ISA unavailable");
    } else
      throw std::runtime_error("unsupported .option");
  }
  void li(const string &rd, uint64_t n) {
    // Balanced base-4096 digits. Unsigned arithmetic handles INT64_MIN.
    int lo = int(n & 4095);
    if (lo >= 2048)
      lo -= 4096;
    if (n == static_cast<uint64_t>(lo)) {
      inst("addi", {rd, "x0", std::to_string(lo)});
      return;
    }
    uint64_t high = (n >> 12) + (lo < 0);
    if (n >> 63)
      high |= (~uint64_t(0) << 52);
    li(rd, high);
    inst("slli", {rd, rd, "12"});
    if (lo)
      inst("addi", {rd, rd, std::to_string(lo)});
  }
  void line(const string &raw) {
    // Strip comments only outside quoted data/directive operands.
    char quote = 0;
    size_t end = 0;
    for (; end < raw.size(); ++end) {
      char c = raw[end];
      if (quote) {
        if (c == '\\' && end + 1 < raw.size())
          ++end;
        else if (c == quote)
          quote = 0;
      } else if (c == '"' || c == '\'')
        quote = c;
      else if (c == '#' || c == ';')
        break;
    }
    string s = trim(raw.substr(0, end));
    auto colon = s.find(':');
    if (colon != s.npos && colon < s.find_first_of(" \t")) {
      string l = s.substr(0, colon);
      if (l.size() == 1 && l[0] >= '0' && l[0] <= '9') {
        unsigned d = l[0] - '0';
        l = local(d, ++labels[d]);
      }
      for (const auto &[alias, target] : aliases)
        if (l == target)
          out += alias + ":\n";
      out += l + ":\n";
      s = trim(s.substr(colon + 1));
    }
    if (s.empty())
      return;
    auto sep = s.find_first_of(" \t");
    string m = s.substr(0, sep);
    string tail = sep == s.npos ? "" : trim(s.substr(sep));
    if (m == ".option") {
      option(split(tail));
      return;
    }
    if (m == ".set" || m == ".type" || m == ".size") {
      auto a = split(tail);
      if (a.size() == 2) {
        if (m == ".set" && aliases.contains(a[0]))
          return;
        for (const auto &[alias, target] : aliases)
          if (a[0] == target) {
            if (m == ".type")
              out += m + " " + alias + ", " + a[1] + "\n";
            if (m == ".size" && a[1] == ".-" + target)
              out += m + " " + alias + ", .-" + alias + "\n";
          }
      }
    }
    if (m.front() == '.') {
      out += s + "\n";
      return;
    }
    auto a = split(tail);
    if ((m == "la" || m == "lla") && a.size() == 2) {
      auto l = prefix + "addr" + std::to_string(++anchor);
      out += l + ":\n";
      bool got = opt.pic && m == "la";
      inst(got ? "rv_got_hi" : "rv_pcrel_hi", {a[0], symbol(a[1])});
      inst(got ? "rv_pcrel_ld" : "rv_pcrel_addi", {a[0], a[0], l});
      return;
    }
    if (m == "li" && a.size() == 2) {
      const string &v = a[1];
      size_t i = 0;
      bool neg = !v.empty() && v[0] == '-';
      if (neg || (!v.empty() && v[0] == '+'))
        ++i;
      int base = 10;
      if (v.compare(i, 2, "0x") == 0 || v.compare(i, 2, "0X") == 0) {
        base = 16;
        i += 2;
      }
      uint64_t n;
      auto [p, e] = std::from_chars(v.data() + i, v.data() + v.size(), n, base);
      check(e == std::errc{} && p == v.data() + v.size() &&
                (!neg || n <= (uint64_t(1) << 63)),
            "invalid 64-bit li constant");
      li(a[0], neg ? 0 - n : n);
      return;
    }
    if (m == "mv" && a.size() == 2) {
      inst("addi", {a[0], a[1], "0"});
      return;
    }
    if (m == "neg" && a.size() == 2) {
      inst("sub", {a[0], "x0", a[1]});
      return;
    }
    if ((m == "beqz" || m == "bnez") && a.size() == 2) {
      inst(m == "beqz" ? "beq" : "bne", {a[0], "x0", symbol(a[1])});
      return;
    }
    if (m == "j" && a.size() == 1) {
      inst("jal", {"x0", symbol(a[0])});
      return;
    }
    if (m == "jr" && a.size() == 1) {
      inst("jalr", {"x0", a[0], "0"});
      return;
    }
    if (m == "jalr" && a.size() == 1) {
      inst("jalr", {"ra", a[0], "0"});
      return;
    }
    if ((m == "beq" || m == "bne" || m == "blt" || m == "bge" || m == "bltu" ||
         m == "bgeu" || m == "jal" || m == "call") &&
        !a.empty())
      a.back() = symbol(a.back());
    if (m == "auipc" && a.size() == 2 && a[1].ends_with(')') &&
        (a[1].starts_with("%pcrel_hi(") ||
         a[1].starts_with("%got_pcrel_hi("))) {
      auto start = a[1].find('(') + 1;
      inst(a[1].starts_with("%got_") ? "rv_got_hi" : "rv_pcrel_hi",
           {a[0], symbol(a[1].substr(start, a[1].size() - start - 1))});
      return;
    }
    if (m == "addi" && a.size() == 3 && a[2].starts_with("%pcrel_lo(") &&
        a[2].ends_with(')')) {
      inst("rv_pcrel_addi",
           {a[0], a[1], symbol(a[2].substr(10, a[2].size() - 11))});
      return;
    }
    static const Args memory = {"ld",  "lw", "lwu", "lh", "lhu", "lb",
                                "lbu", "sd", "sw",  "sh", "sb",  "jalr"};
    if (a.size() == 2 &&
        std::find(memory.begin(), memory.end(), m) != memory.end()) {
      string addr = a[1];
      auto p = addr.rfind('(');
      check(p != addr.npos && addr.back() == ')',
            "expected displacement(base)");
      string off = trim(addr.substr(0, p)),
             base = trim(addr.substr(p + 1, addr.size() - p - 2));
      if ((m == "jalr" || m == "ld") && off.starts_with("%pcrel_lo(") &&
          off.back() == ')') {
        inst(m == "jalr" ? "rv_pcrel_jalr" : "rv_pcrel_ld",
             {a[0], base, symbol(off.substr(10, off.size() - 11))});
        return;
      }
      a = {a[0], base, off.empty() ? "0" : off};
    }
    inst(m, a);
  }
};
} // namespace

extern "C" char *sand_rv_gas(sandstone_asm_t *ctx, const char *src,
                             const char *name) {
  unsigned line = 0;
  try {
    // Custom banks own their syntax. Opt in through the paired-address form.
    uint32_t id, count = 0;
    if (domsymas_find_mnemonic(ctx->bank, "rv_pcrel_hi", &id, 1, &count) !=
            DOMSYMAS_OK ||
        !count)
      return sand_strdup(src);
    Source state(ctx, src);
    std::istringstream lines(src);
    string raw;
    while (std::getline(lines, raw)) {
      ++line;
      state.line(raw);
    }
    check(state.stack.empty(), "unbalanced .option push");
    for (size_t i = 0; i < 10; ++i)
      check(state.forward[i] <= state.labels[i],
            "undefined forward numeric label");
    return sand_strdup(state.out.c_str());
  } catch (const std::exception &e) {
    sand_set_error(ctx, "%s:%u: RV GAS: %s", name, line, e.what());
    return nullptr;
  }
}
