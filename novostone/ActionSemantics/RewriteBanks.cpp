#include "RewriteBanks.hpp"
#include "RewriteBanksInternal.hpp"
#include "ActionSemantics.hpp"
#include "RewriteBankSyntax.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace novostone::action {
namespace {
std::string text(novo_rwb_string_t value) { return {value.data, value.length}; }
std::string unquote(novo_rwb_string_t value) {
  auto raw = text(value);
  std::string result;
  for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
    char c = raw[i];
    if (c == '\\') {
      c = raw[++i];
      if (c == 'n') c = '\n';
      else if (c == 'r') c = '\r';
      else if (c == 't') c = '\t';
    }
    result += c;
  }
  return result;
}
GuardDomain domain(std::string_view name) {
  if (name == "temporal") return GuardDomain::Temporal;
  if (name == "spatial") return GuardDomain::Spatial;
  if (name == "semantic") return GuardDomain::Semantic;
  if (name == "resource_usage") return GuardDomain::ResourceUsage;
  if (name == "resource_limits") return GuardDomain::ResourceLimits;
  throw std::invalid_argument("unknown guard domain '" + std::string(name) + "'");
}
void checkKeys(const BankValue::Object& object,
               std::initializer_list<std::string_view> keys) {
  for (const auto& [key, value] : object) {
    (void)value;
    if (std::find(keys.begin(), keys.end(), key) == keys.end())
      throw std::invalid_argument("unknown contract field '" + key + "'");
  }
}
void appendGuards(std::vector<BankGuard>& into, const BankValue& value,
                  const std::string& source) {
  for (const auto& item : value.array()) {
    const auto& obj = item.object();
    checkKeys(obj, {"kind", "expression"});
    if (!obj.contains("kind") || !obj.contains("expression"))
      throw std::invalid_argument("contract guard requires kind and expression");
    const auto& expression = obj.at("expression").string();
    if (expression.empty()) throw std::invalid_argument("empty contract guard expression");
    into.push_back({domain(obj.at("kind").string()), expression, source, 0});
  }
}
}
const BankValue::Object& BankValue::object() const {
  if (auto p = std::get_if<Object>(&value)) return *p;
  throw std::invalid_argument("expected attachment object");
}
const BankValue::Array& BankValue::array() const {
  if (auto p = std::get_if<Array>(&value)) return *p;
  throw std::invalid_argument("expected attachment array");
}
const std::string& BankValue::string() const {
  if (auto p = std::get_if<std::string>(&value)) return *p;
  throw std::invalid_argument("expected attachment string");
}
void detail::augmentBankContract(RewriteBank& bank, const BankValue& value,
                                const std::string& source) {
  const auto& obj = value.object();
  checkKeys(obj, {"guards", "rules"});
  if (auto it = obj.find("guards"); it != obj.end())
    appendGuards(bank.guards, it->second, source);
  if (auto it = obj.find("rules"); it != obj.end()) {
    for (const auto& [name, rule] : it->second.object()) {
      if (name.empty()) throw std::invalid_argument("empty contract rule name");
      const auto& fields = rule.object();
      checkKeys(fields, {"guards"});
      if (auto g = fields.find("guards"); g != fields.end())
        appendGuards(bank.rule_guards[name], g->second, source);
    }
  }
}
std::vector<BankGuard> RewriteBank::guardsFor(std::string_view rule,
                                            std::vector<BankGuard> result) const {
  result.insert(result.end(), guards.begin(), guards.end());
  if (auto it = rule_guards.find(rule); it != rule_guards.end())
    result.insert(result.end(), it->second.begin(), it->second.end());
  return result;
}
bool RewriteBank::hasRuntimeLua() const {
  return std::any_of(lua.begin(), lua.end(),
    [](const auto& script) { return script.phase == LuaPhase::Runtime; });
}
RewriteBank loadRewriteBank(const std::filesystem::path& path,
                           const BankLoadOptions& options) {
  if (!options.max_file_bytes || !options.max_total_bytes ||
      !options.max_attachment_depth || options.max_attachment_depth > 256)
    throw std::invalid_argument("invalid rewrite bank load limits");
  std::size_t total = 0;
  auto read = [&](const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::invalid_argument(file.string() + ": cannot open bank resource");
    std::string result;
    char buffer[4096];
    while (input.read(buffer, sizeof buffer) || input.gcount()) {
      auto count = static_cast<std::size_t>(input.gcount());
      if (count > options.max_file_bytes - result.size() ||
          count > options.max_total_bytes - total)
        throw std::invalid_argument(file.string() + ": rewrite bank byte limit exceeded");
      result.append(buffer, count); total += count;
    }
    if (!input.eof()) throw std::invalid_argument(file.string() + ": bank resource read failed");
    if (result.find('\0') != std::string::npos)
      throw std::invalid_argument(file.string() + ": NUL byte in bank resource");
    return result;
  };
  auto source = read(path);
  // Match the enclosing ActionSemantics source budget even with relaxed I/O limits.
  if (source.size() > 65536)
    throw ParseError(path.string(), source, 0, "rewrite bank syntax exceeds 65536 bytes");
  std::unique_ptr<novo_rwb_syntax, decltype(&novo_rwb_syntax_free)> syntax(nullptr, novo_rwb_syntax_free);
  {
    static std::mutex parser_mutex;
    std::lock_guard lock(parser_mutex);
    std::size_t error = 0;
    syntax.reset(novo_rwb_parse_syntax(source.c_str(), &error));
    if (!syntax) throw ParseError(path.string(), source, error, "invalid rewrite bank syntax");
  }
  const auto document = novo_rwb_syntax_document(syntax.get());
  RewriteBank bank;
  bank.name = text(document->name); bank.source = path.string();
  std::set<std::string> attachment_names, lua_names;
  auto resource = [&](novo_rwb_string_t name) {
    return (path.parent_path() / unquote(name)).lexically_normal();
  };
  for (std::size_t i = 0; i < document->entries.length; ++i) {
    auto entry = document->entries.items[i];
    try {
      switch (entry->kind) {
      case novo_rwb_Source_kind:
        bank.rewrite_source += read(resource(entry->v.Source.path)) + "\n";
        break;
      case novo_rwb_Rewrite_kind:
        bank.rewrite_source += unquote(entry->v.Rewrite.text) + "\n";
        break;
      case novo_rwb_Data_kind:
      case novo_rwb_Contract_kind: {
        bool contract = entry->kind == novo_rwb_Contract_kind;
        auto name = text(contract ? entry->v.Contract.name : entry->v.Data.name);
        if (!attachment_names.insert(name).second)
          throw std::invalid_argument("duplicate attachment name '" + name + "'");
        auto file = resource(contract ? entry->v.Contract.path : entry->v.Data.path);
        auto format = text(contract ? entry->v.Contract.format : entry->v.Data.format);
        auto value = parseBankAttachment(read(file), format, file.string(), options.max_attachment_depth);
        if (contract) detail::augmentBankContract(bank, value, file.string());
        bank.data.emplace(std::move(name), std::move(value));
        break;
      }
      case novo_rwb_Lua_kind: {
        auto name = text(entry->v.Lua.name);
        if (!lua_names.insert(name).second)
          throw std::invalid_argument("duplicate Lua script name '" + name + "'");
        auto file = resource(entry->v.Lua.path);
        bank.lua.push_back({name, file.string(), read(file),
          text(entry->v.Lua.phase) == "runtime" ? LuaPhase::Runtime : LuaPhase::Evaluation});
        break;
      }
      case novo_rwb_Guard_kind: {
        auto expression = unquote(entry->v.Guard.expression);
        if (expression.empty()) throw std::invalid_argument("empty bank guard expression");
        BankGuard guard{domain(text(entry->v.Guard.domain)), expression, path.string(), entry->offset};
        auto rule = text(entry->v.Guard.rule);
        if (rule.empty()) bank.guards.push_back(std::move(guard));
        else bank.rule_guards[rule].push_back(std::move(guard));
        break;
      }
      default: throw std::invalid_argument("unsupported bank entry");
      }
    } catch (const std::invalid_argument& error) {
      throw ParseError(path.string(), source, entry->offset, error.what());
    }
  }
  return bank;
}
RewriteBank evaluateRewriteBank(const RewriteBank& original, const BankValue& context,
                                const BankLuaOptions& options) {
  auto bank = original;
  for (const auto& script : original.lua) {
    if (script.phase != LuaPhase::Evaluation) continue;
    const auto result = callBankLua(bank, script.name, "evaluate", LuaPhase::Evaluation, context, options);
    const auto& object = result.object();
    checkKeys(object, {"rewrite_source", "guards", "rules", "data"});
    if (auto it = object.find("rewrite_source"); it != object.end())
      bank.rewrite_source += it->second.string() + "\n";
    BankValue::Object contract;
    for (const auto key : {"guards", "rules"})
      if (auto it = object.find(key); it != object.end()) contract.emplace(key, it->second);
    detail::augmentBankContract(bank, BankValue(std::move(contract)), script.source);
    if (auto it = object.find("data"); it != object.end())
      for (const auto& [key, value] : it->second.object()) bank.data.insert_or_assign(key, value);
  }
  return bank;
}
} // namespace novostone::action
