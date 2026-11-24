#include "aurocks/Compiler.hpp"

#include "DiagnosticsEngine.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace aurocks::detail {
namespace {

bool alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool horizontalSpace(char c) {
    return c == ' ' || c == '\t';
}

void report(std::vector<Diagnostic>& diagnostics, Location location,
            const char* code, std::string message) {
    diagnostics.push_back({Severity::error, location, code, std::move(message)});
}

class FaultParser {
public:
    explicit FaultParser(std::string_view input) : input_(input) {}

    std::optional<FaultRule> parse() {
        FaultRule fault;
        spaces();
        auto pattern = quoted();
        if (!pattern) return std::nullopt;
        if (pattern->empty()) return fail("expected a nonempty PEG pattern");
        if (pattern->size() > 4096) return fail("PEG pattern exceeds 4096 bytes");
        fault.pattern = std::move(*pattern);
        spaces();
        if (input_.substr(pos_, 2) != "=>") return fail("expected '=>' before the disposition");
        pos_ += 2;
        spaces();
        auto disposition = word();
        if (disposition == "silent" || disposition == "loud" || disposition == "load") {
            spaces();
            if (word() != "patch") return fail("expected 'patch' after the disposition");
            disposition = disposition == "silent" ? "silent_patch" : "loud_patch";
        }
        if (disposition == "load_patch") disposition = "loud_patch";
        if (disposition != "silent_patch" && disposition != "loud_patch" &&
            disposition != "inform" && disposition != "warn" && disposition != "error")
            return fail("expected silent_patch, loud_patch, inform, warn, or error");
        fault.disposition = disposition;
        spaces();
        if (disposition == "silent_patch" || disposition == "loud_patch") {
            auto replacement = quoted();
            if (!replacement) return std::nullopt;
            fault.replacement = std::move(*replacement);
            spaces();
        }
        if (disposition != "silent_patch") {
            if (disposition == "loud_patch" && peek() != '"') {
                fault.message = "repaired input";
            } else {
                auto message = quoted();
                if (!message) return std::nullopt;
                if (message->empty()) return fail("diagnostic message cannot be empty");
                if (message->size() > 4096) return fail("diagnostic message exceeds 4096 bytes");
                fault.message = std::move(*message);
                spaces();
            }
        }
        if (pos_ != input_.size()) {
            if (word() != "using") return fail("expected 'using' before a C fault handler");
            spaces();
            if (!alpha(peek())) return fail("expected a C handler identifier");
            fault.handler = word();
            spaces();
            if (pos_ != input_.size()) return fail("unexpected text after C fault handler");
        }
        return fault;
    }

    std::string error() const {
        return "invalid %fault at argument byte " + std::to_string(pos_ + 1) + ": " +
            error_ + "; expected %fault \"PEG\" => DISPOSITION ... [using HANDLER]";
    }

private:
    char peek() const { return pos_ < input_.size() ? input_[pos_] : '\0'; }
    void spaces() { while (horizontalSpace(peek())) ++pos_; }
    std::string_view word() {
        const auto begin = pos_;
        while (alpha(peek()) || (peek() >= '0' && peek() <= '9')) ++pos_;
        return input_.substr(begin, pos_ - begin);
    }

    std::nullopt_t fail(const char* error) {
        error_ = error;
        return std::nullopt;
    }

    std::optional<std::string> quoted() {
        if (peek() != '"') return fail("expected a double-quoted string");
        ++pos_;
        std::string result;
        while (pos_ < input_.size()) {
            const char c = peek();
            if (c == '"') {
                ++pos_;
                return result;
            }
            if (c == '\\') {
                ++pos_;
                switch (peek()) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                default: return fail("expected a quoted-string escape (\\\", \\\\, \\n, \\r or \\t)");
                }
            } else {
                if (static_cast<unsigned char>(c) < ' ')
                    return fail("unescaped control character in a quoted string");
                result += c;
            }
            ++pos_;
        }
        return fail("unterminated quoted string");
    }

    std::string_view input_;
    std::size_t pos_ = 0;
    const char* error_ = nullptr;
};

constexpr std::size_t sourceLimit = 64 * 1024;
constexpr std::size_t memoryLimit = 2 * 1024 * 1024;
constexpr std::size_t allocationLimit = 32 * 1024 * 1024;
constexpr int instructionLimit = 100'000;

struct LuaBudget {
    std::size_t live = 0;
    std::size_t allocated = 0;
    bool exceeded = false;
};

void* allocateLua(void* data, void* pointer, std::size_t oldSize, std::size_t size) {
    auto& budget = *static_cast<LuaBudget*>(data);
    // Lua uses oldSize to communicate the object type on fresh allocations.
    if (!pointer) oldSize = 0;
    if (size == 0) {
        std::free(pointer);
        budget.live -= oldSize;
        return nullptr;
    }
    if (size > memoryLimit - (budget.live - oldSize) ||
        (size > oldSize && (budget.exceeded || size > allocationLimit - budget.allocated))) {
        budget.exceeded = true;
        return nullptr;
    }
    auto* result = std::realloc(pointer, size);
    if (!result) return nullptr;
    budget.live = budget.live - oldSize + size;
    if (size > oldSize) budget.allocated += size;
    return result;
}

void limitInstructions(lua_State* state, lua_Debug*) {
    luaL_error(state, "Lua instruction budget exceeded");
}

// The protected Lua calls may longjmp. Their C++ frames use only trivially
// destructible objects. Edits and warnings are staged in fixed buffers owned
// by the unjumpable outer frame, then copied into C++ objects after lua_pcall.
template <std::size_t N>
struct Buffer {
    char data[N]{};
    std::size_t size = 0;
    std::string_view view() const { return {data, size}; }
};

struct SymbolEdit {
    std::size_t rule = 0;
    std::size_t symbol = 0;
    Buffer<4096> text;
};

struct PriorityEdit {
    std::size_t rule = 0;
    int score = 0;
};

struct LuaInvocation {
    LuaInvocation(const Grammar& input, std::string_view script, bool rewrite, bool score)
        : grammar(input), source(script), rewriter(rewrite), scoreRules(score) {}

    const Grammar& grammar;
    std::string_view source;
    bool rewriter;
    bool scoreRules;
    Buffer<256> start{};
    Buffer<64> disambiguator{};
    std::array<SymbolEdit, 16> symbols{};
    std::size_t symbolCount = 0;
    std::array<PriorityEdit, 256> priorities{};
    std::size_t priorityCount = 0;
    std::array<Buffer<512>, 16> warnings{};
    std::size_t warningCount = 0;
};

LuaInvocation& invocation(lua_State* state) {
    return *static_cast<LuaInvocation*>(lua_touserdata(state, lua_upvalueindex(1)));
}

void requireArgs(lua_State* state, int count) {
    if (lua_gettop(state) != count) luaL_error(state, "wrong number of arguments");
}

template <std::size_t N>
void getText(lua_State* state, int index, Buffer<N>& output) {
    luaL_checktype(state, index, LUA_TSTRING);
    std::size_t length;
    const auto* text = lua_tolstring(state, index, &length);
    if (length == 0 || length > N || std::memchr(text, '\0', length))
        luaL_argerror(state, index, "empty, oversized, or contains a NUL byte");
    std::memcpy(output.data, text, length);
    output.size = length;
}

void pushText(lua_State* state, std::string_view text) {
    if (text.size() > sourceLimit) luaL_error(state, "grammar field is too large to inspect");
    lua_pushlstring(state, text.data(), text.size());
}

std::size_t getIndex(lua_State* state, int index, std::size_t count) {
    if (!lua_isinteger(state, index)) luaL_argerror(state, index, "integer index expected");
    const auto value = lua_tointeger(state, index);
    if (value < 1 || static_cast<lua_Unsigned>(value) > count)
        luaL_argerror(state, index, "index out of range");
    return static_cast<std::size_t>(value - 1);
}

void pushSize(lua_State* state, std::size_t size) {
    if (size > static_cast<lua_Unsigned>(LUA_MAXINTEGER))
        luaL_error(state, "grammar field exceeds Lua integer range");
    lua_pushinteger(state, static_cast<lua_Integer>(size));
}

int selectDisambiguator(lua_State* state) {
    requireArgs(state, 1);
    auto& call = invocation(state);
    if (call.scoreRules) return luaL_error(state, "%%disambiguator lua already selects the priority policy");
    auto& policy = call.disambiguator;
    getText(state, 1, policy);
    if (policy.view() != "first" && policy.view() != "error" && policy.view() != "prefer" &&
        policy.view() != "priority")
        return luaL_argerror(state, 1, "expected first, error, prefer, or priority");
    return 0;
}

int setRulePriority(lua_State* state) {
    requireArgs(state, 2);
    auto& call = invocation(state);
    if (!call.scoreRules) return luaL_error(state, "rule scores require %%disambiguator lua");
    const auto rule = getIndex(state, 1, call.grammar.rules.size());
    if (!lua_isinteger(state, 2)) return luaL_argerror(state, 2, "integer score expected");
    const auto score = lua_tointeger(state, 2);
    if (score < std::numeric_limits<int>::min() || score > std::numeric_limits<int>::max())
        return luaL_argerror(state, 2, "score is outside the signed int range");
    for (std::size_t i = 0; i < call.priorityCount; ++i) {
        if (call.priorities[i].rule == rule) {
            call.priorities[i].score = static_cast<int>(score);
            return 0;
        }
    }
    if (call.priorityCount == call.priorities.size())
        return luaL_error(state, "Lua rule score budget exceeded");
    call.priorities[call.priorityCount++] = {rule, static_cast<int>(score)};
    return 0;
}

int setStart(lua_State* state) {
    requireArgs(state, 1);
    auto& call = invocation(state);
    if (!call.rewriter) return luaL_error(state, "grammar edits require %%rewriter lua");
    getText(state, 1, call.start);
    return 0;
}

int getStart(lua_State* state) {
    requireArgs(state, 0);
    const auto& call = invocation(state);
    pushText(state, call.start.size ? call.start.view() : std::string_view(call.grammar.start));
    return 1;
}

int productionCount(lua_State* state) {
    requireArgs(state, 0);
    pushSize(state, invocation(state).grammar.rules.size());
    return 1;
}

int production(lua_State* state) {
    requireArgs(state, 1);
    const auto& rules = invocation(state).grammar.rules;
    const auto& rule = rules[getIndex(state, 1, rules.size())];
    lua_createtable(state, 0, 5);
    pushText(state, rule.lhs);
    lua_setfield(state, -2, "lhs");
    pushSize(state, rule.rhs.size());
    lua_setfield(state, -2, "rhs_count");
    pushSize(state, rule.directives.size());
    lua_setfield(state, -2, "directive_count");
    pushSize(state, rule.location.line);
    lua_setfield(state, -2, "line");
    pushSize(state, rule.location.column);
    lua_setfield(state, -2, "column");
    return 1;
}

int productionDirective(lua_State* state) {
    requireArgs(state, 2);
    const auto& rules = invocation(state).grammar.rules;
    const auto& directives = rules[getIndex(state, 1, rules.size())].directives;
    const auto& directive = directives[getIndex(state, 2, directives.size())];
    pushText(state, directive.name);
    pushText(state, directive.value);
    return 2;
}

SymbolEdit* findEdit(LuaInvocation& call, std::size_t rule, std::size_t symbol) {
    for (std::size_t i = 0; i < call.symbolCount; ++i)
        if (call.symbols[i].rule == rule && call.symbols[i].symbol == symbol) return &call.symbols[i];
    return nullptr;
}

int inspectSymbol(lua_State* state) {
    requireArgs(state, 2);
    auto& call = invocation(state);
    const auto rule = getIndex(state, 1, call.grammar.rules.size());
    const auto& rhs = call.grammar.rules[rule].rhs;
    const auto symbol = getIndex(state, 2, rhs.size());
    const char* kind = "empty";
    switch (rhs[symbol].kind) {
    case SymbolKind::nonterminal: kind = "nonterminal"; break;
    case SymbolKind::literal: kind = "literal"; break;
    case SymbolKind::regex: kind = "regex"; break;
    case SymbolKind::empty: break;
    }
    lua_pushstring(state, kind);
    const auto* edit = findEdit(call, rule, symbol);
    pushText(state, edit ? edit->text.view() : std::string_view(rhs[symbol].text));
    return 2;
}

int replaceSymbol(lua_State* state, SymbolKind kind, const char* expected) {
    requireArgs(state, 3);
    auto& call = invocation(state);
    if (!call.rewriter) return luaL_error(state, "grammar edits require %%rewriter lua");
    const auto rule = getIndex(state, 1, call.grammar.rules.size());
    const auto& rhs = call.grammar.rules[rule].rhs;
    const auto symbol = getIndex(state, 2, rhs.size());
    if (rhs[symbol].kind != kind) return luaL_argerror(state, 2, expected);
    auto* edit = findEdit(call, rule, symbol);
    if (!edit) {
        if (call.symbolCount == call.symbols.size())
            return luaL_error(state, "Lua symbol edit budget exceeded");
        edit = &call.symbols[call.symbolCount++];
        edit->rule = rule;
        edit->symbol = symbol;
    }
    getText(state, 3, edit->text);
    return 0;
}

int replaceLiteral(lua_State* state) {
    return replaceSymbol(state, SymbolKind::literal, "expected a literal terminal");
}

int replaceNonterminal(lua_State* state) {
    return replaceSymbol(state, SymbolKind::nonterminal, "expected a nonterminal");
}

int warn(lua_State* state) {
    requireArgs(state, 1);
    auto& call = invocation(state);
    if (call.warningCount == call.warnings.size())
        return luaL_error(state, "Lua warning budget exceeded");
    getText(state, 1, call.warnings[call.warningCount++]);
    return 0;
}

int runLua(lua_State* state) {
    auto* call = static_cast<LuaInvocation*>(lua_touserdata(state, 1));
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
    const int base = lua_gettop(state);

    // Build a fresh environment from an allowlist, never exposing the real
    // globals or libraries with unmetered pattern engines or I/O. In
    // particular, no protected calls, coroutines, or finalizers are visible
    // to script code, so it cannot catch or escape the instruction hook.
    lua_createtable(state, 0, 7);
    const int environment = lua_gettop(state);
    for (const auto* name : {"assert", "error", "ipairs", "select", "type", "_VERSION"}) {
        lua_getfield(state, base, name);
        lua_setfield(state, environment, name);
    }
    lua_createtable(state, 0, 11);
    static constexpr luaL_Reg functions[] = {
        {"set_start", setStart},
        {"get_start", getStart},
        {"select_disambiguator", selectDisambiguator},
        {"set_rule_priority", setRulePriority},
        {"production_count", productionCount},
        {"production", production},
        {"production_directive", productionDirective},
        {"symbol", inspectSymbol},
        {"replace_literal", replaceLiteral},
        {"replace_nonterminal", replaceNonterminal},
        {"warn", warn},
        {nullptr, nullptr},
    };
    lua_pushlightuserdata(state, call);
    luaL_setfuncs(state, functions, 1);
    lua_setfield(state, environment, "laurocks");

    if (luaL_loadbufferx(state, call->source.data(), call->source.size(), "=aurocks extension", "t") != LUA_OK)
        return lua_error(state);
    lua_pushvalue(state, environment);
    if (!lua_setupvalue(state, -2, 1)) return luaL_error(state, "Lua chunk lacks its sandbox environment");
    lua_sethook(state, limitInstructions, LUA_MASKCOUNT, instructionLimit);
    lua_call(state, 0, 0);
    return 0;
}

std::string safeMessage(std::string_view text) {
    std::string result(text.substr(0, 512));
    for (auto& c : result)
        if (static_cast<unsigned char>(c) < ' ' || c == '\x7f') c = ' ';
    if (text.size() > 512) result += "...";
    return result;
}

bool executeLua(LuaInvocation& call, Location location, std::vector<Diagnostic>& diagnostics) {
    LuaBudget budget;
#if LUA_VERSION_NUM >= 505
    auto* raw = lua_newstate(allocateLua, &budget, luaL_makeseed(nullptr));
#else
    auto* raw = lua_newstate(allocateLua, &budget);
#endif
    if (!raw) {
        report(diagnostics, location, "lua-extension", "could not create the Lua extension state");
        return false;
    }
    const std::unique_ptr<lua_State, decltype(&lua_close)> state(raw, lua_close);
    // Push a light C function (no allocation); initialization is protected.
    lua_pushcfunction(raw, runLua);
    lua_pushlightuserdata(raw, &call);
    const auto status = lua_pcall(raw, 1, 0, 0);
    if (budget.exceeded || status == LUA_ERRMEM) {
        report(diagnostics, location, "lua-extension", "Lua memory or allocation budget exceeded");
        return false;
    }
    if (status != LUA_OK) {
        std::size_t length = 0;
        const char* text = lua_type(raw, -1) == LUA_TSTRING ? lua_tolstring(raw, -1, &length) : nullptr;
        report(diagnostics, location, "lua-extension", text && length ? safeMessage({text, length}) :
               "Lua extension failed without a string error");
        return false;
    }
    return true;
}

} // namespace

void applyExtensions(Grammar& grammar, Options& options, std::vector<Diagnostic>& diagnostics) {
    const Directive* rewriter = nullptr;
    const Directive* disambiguator = nullptr;
    bool valid = true;
    for (const auto& directive : grammar.directives) {
        if (directive.name == "rewriter") {
            if (rewriter) {
                report(diagnostics, directive.location, "rewriter", "duplicate %rewriter directive");
                valid = false;
                continue;
            }
            rewriter = &directive;
            if (directive.value != "lua" && directive.value != "none") {
                report(diagnostics, directive.location, "unsupported-rewriter", "expected %rewriter lua or %rewriter none; other rewriters are unsupported");
                valid = false;
            } else if (directive.value == "lua" && !options.lua_source) {
                report(diagnostics, directive.location, "missing-lua-rewriter", "%rewriter lua requires host-supplied Lua source");
                valid = false;
            }
        } else if (directive.name == "disambiguator") {
            if (disambiguator) {
                report(diagnostics, directive.location, "duplicate-directive", "duplicate %disambiguator directive");
                valid = false;
            } else {
                disambiguator = &directive;
            }
        }
    }
    for (const auto& rule : grammar.rules) {
        for (const auto& directive : rule.directives) {
            if (directive.name != "rewriter") continue;
            report(diagnostics, directive.location, "rewriter", "%rewriter must be declared at grammar scope");
            valid = false;
        }
    }
    const bool luaRewriter = rewriter && rewriter->value == "lua";
    const bool luaScoring = options.disambiguator.empty() ?
        disambiguator && disambiguator->value == "lua" : options.disambiguator == "lua";
    const Location scoreLocation = disambiguator && disambiguator->value == "lua" ?
        disambiguator->location : Location{};
    if (luaScoring && !options.lua_source) {
        report(diagnostics, scoreLocation, "missing-lua-disambiguator", "%disambiguator lua requires host-supplied Lua source");
        valid = false;
    }
    if (!valid || !options.lua_source) return;

    const Location location = luaRewriter ? rewriter->location : scoreLocation;
    const auto& source = *options.lua_source;
    if (source.size() > sourceLimit || source.find('\0') != std::string::npos) {
        report(diagnostics, location, "lua-extension", "Lua source must be text of at most 64 KiB with no NUL bytes");
        return;
    }
    auto invocationStorage = std::make_unique<LuaInvocation>(grammar, source, luaRewriter, luaScoring);
    auto& call = *invocationStorage;
    if (!executeLua(call, location, diagnostics)) return;

    const auto hasProduction = [&](std::string_view name) {
        return std::any_of(grammar.rules.begin(), grammar.rules.end(),
                           [&](const Rule& rule) { return rule.lhs == name; });
    };
    if (call.start.size && !hasProduction(call.start.view())) {
        report(diagnostics, location, "lua-extension", "the Lua start symbol has no production");
        return;
    }
    for (std::size_t i = 0; i < call.symbolCount; ++i) {
        const auto& edit = call.symbols[i];
        const auto& symbol = grammar.rules[edit.rule].rhs[edit.symbol];
        if (symbol.kind == SymbolKind::nonterminal && !hasProduction(edit.text.view())) {
            report(diagnostics, symbol.location, "lua-extension", "the Lua replacement nonterminal has no production");
            return;
        }
    }
    if (call.disambiguator.size && !options.disambiguator.empty() &&
        options.disambiguator != call.disambiguator.view()) {
        report(diagnostics, location, "lua-extension", "Lua may not override an explicit host disambiguator");
        return;
    }
    if (luaScoring && !call.priorityCount) {
        report(diagnostics, scoreLocation, "lua-extension", "%disambiguator lua did not assign any production scores");
        return;
    }
    for (std::size_t i = 0; i < call.priorityCount; ++i) {
        const auto& rule = grammar.rules[call.priorities[i].rule];
        for (const auto& directive : rule.directives) {
            if (directive.name != "priority" && directive.name != "dprec") continue;
            report(diagnostics, directive.location, "lua-extension",
                   "Lua cannot score a production with an explicit %" + directive.name);
            return;
        }
    }

    if (call.start.size) grammar.start = call.start.view();
    for (std::size_t i = 0; i < call.symbolCount; ++i) {
        const auto& edit = call.symbols[i];
        grammar.rules[edit.rule].rhs[edit.symbol].text = edit.text.view();
    }
    if (call.disambiguator.size) options.disambiguator = call.disambiguator.view();
    if (luaScoring) {
        for (std::size_t i = 0; i < call.priorityCount; ++i) {
            const auto& score = call.priorities[i];
            grammar.rules[score.rule].directives.push_back({"priority", std::to_string(score.score), scoreLocation});
        }
        options.disambiguator = "priority";
    }
    for (std::size_t i = 0; i < call.warningCount; ++i)
        diagnostics.push_back({Severity::warning, location, "lua-warning", safeMessage(call.warnings[i].view())});
}

void compileFaultRules(Grammar& grammar, std::vector<Diagnostic>& diagnostics) {
    for (const auto& directive : grammar.directives) {
        if (directive.name != "fault") continue;
        if (grammar.faults.size() == 64) {
            report(diagnostics, directive.location, "fault-syntax", "at most 64 %fault rules are supported");
            return;
        }
        FaultParser parser(directive.value);
        auto fault = parser.parse();
        if (!fault) {
            report(diagnostics, directive.location, "fault-syntax", parser.error());
            continue;
        }
        try {
            (void)firestone::lr::LRDiagBytePEG(fault->pattern);
            fault->location = directive.location;
            grammar.faults.push_back(std::move(*fault));
        } catch (const std::invalid_argument& error) {
            report(diagnostics, directive.location, "fault-peg", error.what());
        }
    }
}

} // namespace aurocks::detail
