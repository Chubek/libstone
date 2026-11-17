#include "ScriptSDK.hpp"
#include "KaguyaCompat.hpp"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace qld {
namespace {
constexpr size_t limit = 16 * 1024 * 1024;
std::string trim(const std::string &s) {
    auto first = s.find_first_not_of(" \r\n\t");
    return first == s.npos ? "" : s.substr(first, s.find_last_not_of(" \r\n\t") - first + 1);
}
bool identifier(const std::string &s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (unsigned char c : s) if (!std::isalnum(c) && c != '_') return false;
    return true;
}
std::vector<std::string> arguments(const std::string &s) {
    std::vector<std::string> out;
    if (trim(s).empty()) return out;
    char quote = 0; int depth = 0; size_t begin = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i == s.size() ? 0 : s[i];
        if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') { if (--depth < 0) throw std::runtime_error("unbalanced argument"); }
        else if ((!c || c == ',') && !depth) { out.push_back(trim(s.substr(begin, i - begin))); begin = i + 1; }
    }
    if (quote || depth) throw std::runtime_error("unterminated argument");
    return out;
}
void append(std::string &out, const std::string &text) {
    if (text.size() > limit - out.size() || text.find('\0') != text.npos) throw std::runtime_error("invalid or oversized generated script");
    out += text;
}
}
void ScriptSDK::registerDirective(std::string name, Directive callback) {
    if (!identifier(name) || !callback || directives_.count(name)) throw std::runtime_error("invalid or duplicate script directive: " + name);
    directives_.emplace(std::move(name), std::move(callback));
}
void ScriptSDK::defineMacro(std::string name, Arguments params, std::string body) {
    std::set<std::string> names;
    for (const auto &p : params) if (!identifier(p) || !names.insert(p).second) throw std::runtime_error("invalid macro parameter: " + p);
    registerDirective(std::move(name), [params = std::move(params), body = std::move(body)](const Arguments &args) {
        if (args.size() != params.size()) throw std::runtime_error("macro argument count mismatch");
        std::string out;
        for (size_t i = 0; i < body.size();) {
            if (body.compare(i, 2, "${") != 0) { append(out, body.substr(i++, 1)); continue; }
            auto end = body.find('}', i + 2);
            if (end == body.npos) throw std::runtime_error("unterminated macro substitution");
            auto p = body.substr(i + 2, end - i - 2); size_t index = 0;
            while (index < params.size() && params[index] != p) ++index;
            if (index == params.size()) throw std::runtime_error("unknown macro parameter: " + p);
            append(out, args[index]); i = end + 1;
        }
        return out;
    });
}
std::string ScriptSDK::preprocess(const std::string &source, const std::string &name) const {
    if (source.starts_with("--!lua") || std::filesystem::path(name).extension() == ".lua") return lua(source, name);
    size_t steps = 0;
    std::set<std::filesystem::path> included;
    std::function<std::string(const std::string &, const std::string &, unsigned)> expand;
    expand = [&](const std::string &src, const std::string &file, unsigned depth) {
        if (depth > 64 || src.size() > limit || src.find('\0') != src.npos) throw std::runtime_error("script expansion limit exceeded");
        std::string out, line; std::istringstream in(src); size_t lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            try {
                if (++steps > 100000) throw std::runtime_error("script expansion step limit exceeded");
                auto s = trim(line);
                if (s.starts_with("INCLUDE ") || s.starts_with("INCLUDE\t")) {
                    auto path = trim(s.substr(7));
                    if (path.size() > 1 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
                    auto full = std::filesystem::weakly_canonical(std::filesystem::path(file).parent_path() / path);
                    if (!included.insert(full).second) throw std::runtime_error("script include cycle");
                    std::ifstream input(full, std::ios::binary | std::ios::ate);
                    if (!input || input.tellg() < 0 || input.tellg() > static_cast<std::streamoff>(limit)) throw std::runtime_error("cannot read include: " + full.string());
                    std::string text(static_cast<size_t>(input.tellg()), '\0'); input.seekg(0);
                    if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("include read failed");
                    append(out, expand(text, full.string(), depth + 1)); included.erase(full);
                } else if (s.starts_with('@')) {
                    auto end = s.find_first_of(" \t"); auto key = s.substr(1, end == s.npos ? s.size() - 1 : end - 1);
                    auto it = directives_.find(key);
                    if (it == directives_.end()) throw std::runtime_error("unknown script directive: " + key);
                    append(out, expand(it->second(arguments(end == s.npos ? "" : s.substr(end))), file, depth + 1));
                } else { append(out, line); append(out, "\n"); }
            } catch (const std::exception &e) { throw std::runtime_error(file + ":" + std::to_string(lineno) + ": " + e.what()); }
        }
        return out;
    };
    return expand(source, name, 0);
}
std::string ScriptSDK::lua(const std::string &source, const std::string &name) const {
    kaguya::State state; std::string out, error;
    state.setErrorHandler([&](int, const char *message) { error = message ? message : "Lua error"; });
    state["qld"] = kaguya::NewTable(); auto api = state["qld"].get<kaguya::LuaTable>();
    api["emit"] = kaguya::function([&](const std::string &text) { append(out, text); append(out, "\n"); });
    api["directive"] = kaguya::function([&](const std::string &key, const std::vector<std::string> &args) {
        auto it = directives_.find(key);
        if (it == directives_.end()) throw std::runtime_error("unknown script directive: " + key);
        append(out, it->second(args)); append(out, "\n");
    });
    if (!state.dostring(R"lua(
local function name(s)
  assert(type(s) == 'string' and s:match('^[%a_%.][%w_%.%$]*$'), 'invalid linker name')
  return s
end
local function quote(s)
  assert(type(s) == 'string' and not s:find('[\r\n%z]'), 'invalid linker string')
  return '"' .. s:gsub('\\', '\\\\'):gsub('"', '\\"') .. '"'
end
local function expr(v)
  assert(type(v) == 'string' or math.type(v) == 'integer', 'expected linker expression or integer')
  return tostring(v)
end
function qld.entry(s) qld.emit('ENTRY(' .. name(s) .. ')') end
function qld.input(s) qld.emit('INPUT(' .. quote(s) .. ')') end
function qld.search_dir(s) qld.emit('SEARCH_DIR(' .. quote(s) .. ')') end
function qld.define(s, value) qld.emit(name(s) .. ' = ' .. expr(value) .. ';') end
function qld.provide(s, value) qld.emit('PROVIDE(' .. name(s) .. ' = ' .. expr(value) .. ');') end
function qld.memory(s, origin, length)
  qld.emit('MEMORY { ' .. name(s) .. ' : ORIGIN = ' .. expr(origin) .. ', LENGTH = ' .. expr(length) .. ' }')
end
function qld.section(s, pattern, address, region)
  assert(type(pattern) == 'string' and not pattern:find('[{};%z]'), 'invalid section pattern')
  qld.emit('SECTIONS { ' .. name(s) .. ' ' .. (address and expr(address) or '') ..
    ' : { *(' .. pattern .. ') } ' .. (region and '>' .. name(region) or '') .. ' }')
end
function qld.assertion(condition, message)
  qld.emit('ASSERT(' .. expr(condition) .. ', ' .. quote(message) .. ');')
end
function qld.macro(name, callback)
  assert(type(callback) == 'function' and qld[name] == nil, 'invalid or duplicate Lua macro')
  qld[name] = callback
end
)lua") || !state.dostring(source) || !error.empty()) throw std::runtime_error(name + ": " + error);
    // Use a non-.lua name so emitted script is never accidentally executed again.
    if (out.starts_with("--!lua")) throw std::runtime_error("Lua must emit linker script commands, not another Lua script");
    return preprocess(out, name + ".ld");
}
}
