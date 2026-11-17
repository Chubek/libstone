#include "intern.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr size_t limit = 16 * 1024 * 1024;
std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    return first == s.npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
bool ident(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$'; }
std::string uncomment(const std::string &s) {
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (quote) { if (s[i] == '\\') ++i; else if (s[i] == quote) quote = 0; }
        else if (s[i] == '"' || s[i] == '\'') quote = s[i];
        else if (s[i] == ';') return s.substr(0, i);
    }
    return s;
}
std::pair<std::string, std::string> command(const std::string &s) {
    auto text = trim(uncomment(s));
    auto end = text.find_first_of(" \t");
    return {text.substr(0, end), end == text.npos ? "" : trim(text.substr(end))};
}
std::vector<std::string> arguments(const std::string &s) {
    if (trim(s).empty()) return {};
    std::vector<std::string> out;
    size_t start = 0; int depth = 0; char quote = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i == s.size() ? 0 : s[i];
        if (quote) { if (c == '\\') ++i; else if (c == quote) quote = 0; }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') { if (--depth < 0) throw std::runtime_error("unbalanced argument"); }
        else if ((!c || c == ',') && depth == 0) { out.push_back(trim(s.substr(start, i - start))); start = i + 1; }
    }
    if (depth || quote) throw std::runtime_error("unterminated argument");
    return out;
}
std::string read(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < 0 || in.tellg() > static_cast<std::streamoff>(limit))
        throw std::runtime_error("cannot read file (maximum 16 MiB): " + path.string());
    std::string out(static_cast<size_t>(in.tellg()), '\0'); in.seekg(0);
    if (!out.empty() && !in.read(out.data(), static_cast<std::streamsize>(out.size())))
        throw std::runtime_error("file read failed: " + path.string());
    return out;
}
struct Line { std::string text, file; size_t number; };
std::vector<Line> lines(const std::string &src, const std::string &file, size_t first = 1) {
    if (src.size() > limit || src.find('\0') != src.npos) throw std::runtime_error("invalid or oversized source");
    std::vector<Line> out; std::istringstream in(src); std::string s;
    while (std::getline(in, s)) out.push_back({s, file, first++});
    return out;
}
struct Parameter { std::string name, fallback; bool optional = false, variadic = false; };
struct Macro { std::vector<Parameter> params; std::vector<Line> body; };
// Recognize the SandAS delimiter only outside Lua strings and comments,
// including Lua's arbitrary-level long brackets. Scan each line once.
std::string luaBlock(const std::vector<Line> &src, size_t &line) {
    std::string code, close;
    char quote = 0;
    for (bool first = true; line < src.size(); ++line, first = false) {
        auto text = first ? trim(src[line].text).substr(2) : src[line].text;
        for (size_t i = 0; i < text.size(); ++i) {
            if (!close.empty()) {
                if (text.compare(i, close.size(), close) == 0) { i += close.size() - 1; close.clear(); }
                continue;
            }
            if (quote) {
                if (text[i] == '\\') ++i;
                else if (text[i] == quote) quote = 0;
                continue;
            }
            if (text.compare(i, 2, ":>") == 0) {
                if (!trim(uncomment(text.substr(i + 2))).empty()) throw std::runtime_error("unexpected text after :>");
                code += text.substr(0, i);
                return code;
            }
            if (text[i] == '\'' || text[i] == '"') { quote = text[i]; continue; }
            bool comment = text.compare(i, 2, "--") == 0;
            size_t start = i + (comment ? 2 : 0);
            if (start < text.size() && text[start] == '[') {
                size_t end = start + 1;
                while (end < text.size() && text[end] == '=') ++end;
                if (end < text.size() && text[end] == '[') {
                    close = "]" + text.substr(start + 1, end - start - 1) + "]";
                    i = end; continue;
                }
            }
            if (comment) break;
        }
        code += text; code += '\n';
        if (code.size() > limit) throw std::runtime_error("Lua block exceeds 16 MiB");
    }
    throw std::runtime_error("unterminated Lua block");
}
class Processor {
    sandstone_asm_t *ctx;
    std::map<std::string, int64_t> constants;
    std::map<std::string, Macro> macros;
    std::set<std::filesystem::path> includes;
    std::vector<std::string> sections;
    std::string section = ".text", previous = ".text", output;
    size_t steps = 0, unique = 0;
    void emit(const std::string &s) {
        if (s.size() + 1 > limit - output.size()) throw std::runtime_error("expanded source exceeds 16 MiB");
        output += s; output += '\n';
    }
    std::string expandConstants(const std::string &expr) {
        std::string result;
        for (size_t i = 0; i < expr.size();) {
            if (std::isalpha(static_cast<unsigned char>(expr[i])) || expr[i] == '_' || expr[i] == '.') {
                size_t end = i + 1; while (end < expr.size() && ident(expr[end])) ++end;
                auto name = expr.substr(i, end - i); auto it = constants.find(name);
                result += it == constants.end() ? name : "(" + std::to_string(it->second) + ")";
                i = end;
            } else if (std::isdigit(static_cast<unsigned char>(expr[i]))) {
                size_t end = i + 1; while (end < expr.size() && ident(expr[end])) ++end;
                result += expr.substr(i, end - i); i = end;
            } else result += expr[i++];
        }
        return result;
    }
    int64_t eval(const std::string &expr) {
        int ok = 0; auto text = expandConstants(expr);
        auto value = sand_eval_expr(ctx, text.c_str(), &ok);
        if (!ok) throw std::runtime_error("unresolved or invalid constant expression: " + expr);
        return value;
    }
    void define(const std::string &name, int64_t value, bool replace = false) {
        if (!sand_is_name(name.c_str())) throw std::runtime_error("invalid constant name: " + name);
        if (!replace && constants.count(name)) throw std::runtime_error("duplicate constant: " + name);
        constants[name] = value;
        sand_lua_constant(ctx, name.c_str(), value);
        emit(".equ " + name + ", " + std::to_string(value));
    }
    static size_t blockEnd(const std::vector<Line> &src, size_t start,
                           const std::string &open, const std::string &close) {
        size_t depth = 0;
        for (size_t i = start + 1; i < src.size(); ++i) {
            if (trim(src[i].text).starts_with("<:")) { luaBlock(src, i); continue; }
            auto cmd = command(src[i].text).first;
            if (cmd == open) ++depth;
            if (cmd == close || (close == ".endmacro" && cmd == ".endm")) {
                if (!depth) return i;
                --depth;
            }
        }
        throw std::runtime_error("unterminated " + open);
    }
    static std::vector<Line> body(const std::vector<Line> &src, size_t begin, size_t end) {
        return {src.begin() + static_cast<std::ptrdiff_t>(begin), src.begin() + static_cast<std::ptrdiff_t>(end)};
    }
    std::string substitute(const std::string &text, const std::map<std::string, std::string> &bindings,
                           const std::map<std::string, std::string> &locals, size_t id) {
        std::string out; char quote = 0;
        for (size_t i = 0; i < text.size();) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                if (text[i + 1] == '@') { out += std::to_string(id); i += 2; continue; }
                size_t end = i + 1; while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
                auto it = bindings.find(text.substr(i + 1, end - i - 1));
                if (it != bindings.end()) { out += it->second; i = end; continue; }
                out += text[i++]; if (i < text.size()) out += text[i++]; continue;
            }
            if (text[i] == '"' || text[i] == '\'') { if (!quote) quote = text[i]; else if (quote == text[i]) quote = 0; }
            if (!quote && ident(text[i])) {
                size_t end = i + 1; while (end < text.size() && ident(text[end])) ++end;
                auto name = text.substr(i, end - i); auto it = locals.find(name);
                out += it == locals.end() ? name : it->second; i = end;
            } else out += text[i++];
        }
        return out;
    }
    std::filesystem::path path(const std::string &arg, const Line &line) {
        if (arg.size() < 2 || arg.front() != '"' || arg.back() != '"') throw std::runtime_error("expected quoted file path");
        auto p = std::filesystem::path(arg.substr(1, arg.size() - 2));
        return p.is_absolute() ? p : std::filesystem::path(line.file).parent_path() / p;
    }
public:
    explicit Processor(sandstone_asm_t *context) : ctx(context) {}
    void process(const std::vector<Line> &src, unsigned recursion = 0) {
        if (recursion > 64) throw std::runtime_error("macro/include expansion depth exceeds 64");
        struct Condition { bool parent, taken, otherwise; };
        std::vector<Condition> conditions; bool active = true;
        for (size_t i = 0; i < src.size(); ++i) {
            const auto &line = src[i];
            try {
                if (++steps > 100000) throw std::runtime_error("source expansion exceeds 100000 steps");
                auto [cmd, arg] = command(line.text);
                if (cmd == ".if" || cmd == ".ifdef" || cmd == ".ifndef") {
                    bool value = false;
                    if (active) value = cmd == ".if" ? eval(arg) != 0 : (constants.count(arg) || macros.count(arg));
                    if (active && cmd == ".ifndef") value = !value;
                    conditions.push_back({active, value, false}); active = active && value; continue;
                }
                if (cmd == ".else" || cmd == ".elseif") {
                    if (conditions.empty() || conditions.back().otherwise) throw std::runtime_error("unexpected " + cmd);
                    auto &c = conditions.back();
                    active = c.parent && !c.taken && (cmd == ".else" || eval(arg) != 0);
                    c.taken = c.taken || active; c.otherwise = cmd == ".else"; continue;
                }
                if (cmd == ".endif") {
                    if (conditions.empty()) throw std::runtime_error("unexpected .endif");
                    active = conditions.back().parent; conditions.pop_back(); continue;
                }
                // Lua is collected before interpreting assembly directives/comments in its body.
                auto raw = trim(line.text);
                if (raw.starts_with("<:")) {
                    std::string code = luaBlock(src, i);
                    if (active) {
                        char *text = sand_run_lua_block(ctx, code.c_str());
                        if (!text) throw std::runtime_error(ctx->error);
                        std::string generated(text); free(text);
                        process(lines(generated, line.file, line.number), recursion + 1);
                    }
                    continue;
                }
                bool block = cmd == ".macro" || cmd == ".rept" || cmd == ".struct" || cmd == ".enum";
                if (!active) {
                    if (block) i = blockEnd(src, i, cmd, cmd == ".macro" ? ".endmacro" : cmd == ".rept" ? ".endr" : cmd == ".struct" ? ".endstruct" : ".endenum");
                    continue;
                }
                if (cmd.empty()) { emit(""); continue; }
                if (cmd.ends_with(':')) {
                    emit(cmd);
                    process(lines(arg, line.file, line.number), recursion + 1);
                    continue;
                }
                if (cmd == ".include") {
                    auto file = std::filesystem::weakly_canonical(path(arg, line));
                    if (!includes.insert(file).second) throw std::runtime_error("include cycle: " + file.string());
                    process(lines(read(file), file.string()), recursion + 1); includes.erase(file); continue;
                }
                if (cmd == ".macro") {
                    auto [name, params] = command(arg);
                    if (!sand_is_name(name.c_str()) || macros.count(name)) throw std::runtime_error("invalid or duplicate macro: " + name);
                    Macro m;
                    std::set<std::string> names;
                    for (auto text : arguments(params)) {
                        Parameter p; auto eq = text.find('=');
                        p.name = trim(text.substr(0, eq)); p.optional = eq != text.npos;
                        if (p.optional) p.fallback = trim(text.substr(eq + 1));
                        p.variadic = p.name.ends_with("..."); if (p.variadic) p.name.resize(p.name.size() - 3);
                        if (!sand_is_name(p.name.c_str()) || !names.insert(p.name).second || (!m.params.empty() && m.params.back().variadic))
                            throw std::runtime_error("invalid macro parameters");
                        m.params.push_back(std::move(p));
                    }
                    auto end = blockEnd(src, i, cmd, ".endmacro"); m.body = body(src, i + 1, end);
                    macros.emplace(name, std::move(m)); i = end; continue;
                }
                if (cmd == ".purge") { if (!macros.erase(arg)) throw std::runtime_error("unknown macro: " + arg); continue; }
                if (cmd == ".rept") {
                    auto count = eval(arg); if (count < 0 || count > 100000) throw std::runtime_error("invalid .rept count");
                    auto end = blockEnd(src, i, cmd, ".endr"); auto repeat = body(src, i + 1, end);
                    for (int64_t n = 0; n < count; ++n) process(repeat, recursion + 1);
                    i = end; continue;
                }
                if (cmd == ".struct" || cmd == ".enum") {
                    if (!sand_is_name(arg.c_str())) throw std::runtime_error("invalid structure/enum name");
                    auto end = blockEnd(src, i, cmd, cmd == ".struct" ? ".endstruct" : ".endenum");
                    int64_t offset = 0;
                    for (size_t j = i + 1; j < end; ++j) {
                        auto [op, rest] = command(src[j].text); if (op.empty()) continue;
                        if (cmd == ".struct") {
                            if (op == ".field") {
                                auto a = arguments(rest); if (a.size() != 2) throw std::runtime_error(".field expects NAME,SIZE");
                                auto size = eval(a[1]); if (size < 0 || size > INT64_MAX - offset) throw std::runtime_error("invalid field size");
                                define(arg + "." + a[0], offset); offset += size;
                            } else if (op == ".align") {
                                auto a = eval(rest); if (a <= 0 || offset > INT64_MAX - a) throw std::runtime_error("invalid struct alignment");
                                offset = (offset + a - 1) / a * a;
                            } else throw std::runtime_error("unknown structure directive: " + op);
                        } else {
                            auto entry = trim(uncomment(src[j].text)); auto eq = entry.find('=');
                            auto name = trim(entry.substr(0, eq)); if (eq != entry.npos) offset = eval(entry.substr(eq + 1));
                            define(name, offset); define(arg + "." + name, offset);
                            if (offset == INT64_MAX) throw std::runtime_error("enum overflow");
                            ++offset;
                        }
                    }
                    if (cmd == ".struct") define(arg + ".size", offset);
                    i = end; continue;
                }
                if (cmd == ".endmacro" || cmd == ".endm" || cmd == ".endr" || cmd == ".endstruct" || cmd == ".endenum")
                    throw std::runtime_error("unexpected " + cmd);
                if (cmd == ".equ" || cmd == ".set") {
                    auto a = arguments(arg); if (a.size() != 2) throw std::runtime_error(cmd + " expects NAME,EXPR");
                    int ok = 0; auto expr = expandConstants(a[1]); auto value = sand_eval_expr(ctx, expr.c_str(), &ok);
                    if (ok) {
                        if (cmd == ".equ" && constants.count(a[0])) throw std::runtime_error("duplicate constant: " + a[0]);
                        constants[a[0]] = value; sand_lua_constant(ctx, a[0].c_str(), value);
                        // Freeze .set uses at their source position; forward .equ remains a layout expression.
                        emit(cmd + " " + a[0] + ", " + std::to_string(value));
                    } else emit(cmd + " " + a[0] + ", " + expr);
                    continue;
                }
                if (cmd == ".error") throw std::runtime_error(arg);
                if (cmd == ".assert") {
                    auto a = arguments(arg); if (a.empty() || a.size() > 2) throw std::runtime_error(".assert expects EXPR[,MESSAGE]");
                    if (!eval(a[0])) throw std::runtime_error(a.size() == 2 ? a[1] : "assembly assertion failed");
                    continue;
                }
                if (cmd == ".incbin") {
                    auto a = arguments(arg); if (a.empty() || a.size() > 3) throw std::runtime_error(".incbin expects FILE[,OFFSET[,COUNT]]");
                    auto bytes = read(path(a[0], line)); auto off = a.size() > 1 ? eval(a[1]) : 0;
                    auto count = a.size() > 2 ? eval(a[2]) : static_cast<int64_t>(bytes.size()) - off;
                    if (off < 0 || count < 0 || static_cast<uint64_t>(off) > bytes.size() || static_cast<uint64_t>(count) > bytes.size() - off)
                        throw std::runtime_error(".incbin range exceeds file");
                    for (int64_t n = 0; n < count; ++n) emit(".byte " + std::to_string(static_cast<unsigned char>(bytes[off + n])));
                    continue;
                }
                auto macro = macros.find(cmd);
                if (macro != macros.end()) {
                    const auto m = macro->second; auto a = arguments(arg);
                    std::map<std::string, std::string> bindings, locals; size_t pos = 0, id = ++unique;
                    for (const auto &p : m.params) {
                        std::string value;
                        if (p.variadic) { while (pos < a.size()) { if (!value.empty()) value += ", "; value += a[pos++]; } }
                        else if (pos < a.size() && !a[pos].empty()) value = a[pos++];
                        else if (p.optional) { value = p.fallback; if (pos < a.size()) ++pos; }
                        else throw std::runtime_error("missing macro argument: " + p.name);
                        bindings[p.name] = value;
                        bindings[std::to_string(bindings.size() / 2 + 1)] = value;
                    }
                    if (pos < a.size()) throw std::runtime_error("too many macro arguments");
                    for (const auto &b : m.body) {
                        auto [op, rest] = command(b.text);
                        if (op == "local") for (const auto &name : arguments(rest)) {
                            if (!sand_is_name(name.c_str())) throw std::runtime_error("invalid local label");
                            locals[name] = "__sand_macro_" + std::to_string(id) + "_" + name;
                        }
                    }
                    auto expanded = m.body;
                    for (auto &b : expanded) b.text = command(b.text).first == "local" ? "" : substitute(b.text, bindings, locals, id);
                    process(expanded, recursion + 1); continue;
                }
                if (cmd == ".pushsection") { sections.push_back(section); cmd = ".section"; }
                if (cmd == ".popsection") {
                    if (sections.empty()) throw std::runtime_error("empty section stack");
                    arg = sections.back(); sections.pop_back(); cmd = ".section";
                }
                if (cmd == ".previous") { arg = previous; cmd = ".section"; }
                if (cmd == ".section" || cmd == ".text" || cmd == ".data" || cmd == ".rodata" || cmd == ".bss") {
                    previous = section; section = cmd == ".section" ? arg : cmd;
                    emit(cmd + " " + arg); continue;
                }
                char *generated = nullptr;
                auto extensionArgs = cmd == ".insn" ? expandConstants(arg) : arg;
                int handled = sand_extension_expand(ctx, cmd.c_str(), extensionArgs.c_str(), &generated);
                if (handled < 0) throw std::runtime_error(ctx->error);
                if (handled) {
                    std::string text(generated); free(generated);
                    process(lines(text, line.file, line.number), recursion + 1); continue;
                }
                const std::set<std::string> numeric = {".byte", ".short", ".hword", ".2byte", ".word", ".long", ".4byte", ".quad", ".8byte", ".space", ".skip", ".zero", ".fill", ".align", ".balign", ".p2align", ".org", ".uleb128", ".sleb128"};
                if (numeric.count(cmd)) {
                    auto values = arguments(arg); std::string args;
                    for (const auto &value : values) {
                        if (!args.empty()) args += ", ";
                        args += expandConstants(value);
                    }
                    emit(cmd + " " + args);
                } else emit(uncomment(line.text));
            } catch (const std::exception &e) {
                throw std::runtime_error(line.file + ":" + std::to_string(line.number) + ": " + e.what());
            }
        }
        if (!conditions.empty()) throw std::runtime_error("unterminated .if");
    }
    const std::string &result() const { return output; }
};
}
char *sand_preprocess_source(sandstone_asm_t *ctx, const char *src, const char *name) {
    try {
        Processor processor(ctx);
        char *prelude = sand_extension_prelude(ctx);
        if (!prelude) return nullptr;
        std::string definitions(prelude); free(prelude);
        processor.process(lines(definitions, "<SDK>"));
        processor.process(lines(src ? src : "", name ? name : "<string>"));
        return sand_strdup(processor.result().c_str());
    } catch (const std::exception &e) { sand_set_error(ctx, "%s", e.what()); return nullptr; }
}
