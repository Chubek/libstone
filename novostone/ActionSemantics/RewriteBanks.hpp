#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace novostone::action {
// Attachment and Lua interchange values are owned, recursive JSON-shaped data.
struct BankValue {
  using Array = std::vector<BankValue>;
  using Object = std::map<std::string, BankValue, std::less<>>;
  using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double,
                               std::string, Array, Object>;
  Storage value = nullptr;
  BankValue() = default;
  template<class T> explicit BankValue(T v) : value(std::move(v)) {}
  const Object& object() const;
  const Array& array() const;
  const std::string& string() const;
};
enum class GuardDomain { Temporal, Spatial, Semantic, ResourceUsage, ResourceLimits };
struct BankGuard {
  GuardDomain domain = GuardDomain::Semantic;
  std::string expression;
  std::string source;
  std::size_t offset = 0;
};
enum class LuaPhase { Evaluation, Runtime };
struct BankLuaScript {
  std::string name, source, text;
  LuaPhase phase = LuaPhase::Evaluation;
};
struct RewriteBank {
  std::string name, source, rewrite_source;
  BankValue::Object data;
  std::vector<BankGuard> guards;
  std::map<std::string, std::vector<BankGuard>, std::less<>> rule_guards;
  std::vector<BankLuaScript> lua;
  // Conjunction order: explicit guards, bank guards, then rule contracts.
  std::vector<BankGuard> guardsFor(std::string_view rule,
                                 std::vector<BankGuard> explicit_guards = {}) const;
  bool hasRuntimeLua() const;
};
struct BankLoadOptions {
  std::size_t max_file_bytes = 65536;
  std::size_t max_total_bytes = 1048576;
  std::size_t max_attachment_depth = 64;
};
struct BankLuaOptions {
  std::size_t max_instructions = 100000;
  std::size_t max_memory_bytes = 8388608;
  std::size_t max_value_depth = 64;
};
// Loading never executes Lua. Paths in a bank are relative to the bank file.
// Errors are ParseError (bank syntax) or invalid_argument (I/O/schema/Lua).
RewriteBank loadRewriteBank(const std::filesystem::path& path,
                           const BankLoadOptions& options = {});
BankValue parseBankAttachment(std::string_view text, std::string_view format,
                              std::string source = "<attachment>",
                              std::size_t max_depth = 64);
std::string bankValueJson(const BankValue& value);
// Fresh bounded vendored Lua VM per call. Phase mismatch is an error.
// Lua receives one context argument and global `data` attachment table.
BankValue callBankLua(const RewriteBank& bank, std::string_view script,
                      std::string_view function, LuaPhase phase,
                      const BankValue& context = {},
                      const BankLuaOptions& options = {});
// Executes only evaluation scripts' evaluate(context), in declaration order.
// Each returns an object with optional rewrite_source, guards, rules, and data;
// effects append/augment the returned bank. Runtime scripts are retained.
RewriteBank evaluateRewriteBank(const RewriteBank& bank,
                                const BankValue& context = {},
                                const BankLuaOptions& options = {});
} // namespace novostone::action
