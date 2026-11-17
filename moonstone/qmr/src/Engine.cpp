#include "qmr/Qmr.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
// The in-tree Lua may be 5.5; adapt the legacy Kaguya declarations locally
// rather than changing either vendored dependency.
#if LUA_VERSION_NUM >= 505
#include <cstdint>
inline lua_State *qmr_newstate(lua_Alloc allocator, void *user) {
    return lua_newstate(allocator, user, 0);
}
#define LUA_GCSETPAUSE (-10001)
#define LUA_GCSETSTEPMUL (-10002)
inline int qmr_gc(lua_State *L, int what, int value) {
    if (what == LUA_GCSETPAUSE) return lua_gc(L, LUA_GCPARAM, LUA_GCPPAUSE, value);
    if (what == LUA_GCSETSTEPMUL) return lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPMUL, value);
    if (what == LUA_GCSTEP) {
        auto bytes = value > 0 ? static_cast<std::size_t>(value) : 0;
        bytes = bytes > SIZE_MAX / 1024 ? SIZE_MAX : bytes * 1024;
        return lua_gc(L, what, bytes);
    }
    return lua_gc(L, what);
}
#define lua_newstate(allocator, user) qmr_newstate(allocator, user)
#define lua_gc(L, what, value) qmr_gc(L, what, value)
#endif
#include <kaguya/kaguya.hpp>
#if LUA_VERSION_NUM >= 505
#undef lua_newstate
#undef lua_gc
#undef LUA_GCSETPAUSE
#undef LUA_GCSETSTEPMUL
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <regex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace moonstone::qmr {

// ---------------------------------------------------------------------------
// Compiled-template cache
// ---------------------------------------------------------------------------
struct ProgramCache::Impl {
    using Key = std::pair<std::string, std::string>; // (name, text)

    mutable std::mutex mutex;
    std::map<Key, std::shared_ptr<const Program>> entries;
    std::deque<Key> order; // insertion order, oldest first
    std::size_t max_bytes = 0;
    std::size_t total_bytes = 0;

    static std::size_t cost(const Key &key, const Program &program) {
        // Approximate in-memory footprint; line_map entries are the bulk
        // alongside the generated Lua source.
        return key.first.size() + key.second.size() + program.lua_source.size() +
               program.line_map.size() * sizeof(LineOrigin);
    }
    void evict_over_budget() {
        while (total_bytes > max_bytes && !order.empty()) {
            const Key key = order.front();
            order.pop_front();
            auto it = entries.find(key);
            if (it == entries.end()) continue;
            total_bytes -= cost(key, *it->second);
            entries.erase(it);
        }
    }
};

ProgramCache::ProgramCache(std::size_t max_bytes) : impl_(new Impl) {
    impl_->max_bytes = max_bytes == 0 ? std::size_t{1} : max_bytes;
}

std::shared_ptr<const Program> ProgramCache::get(const Source &source, const Limits &limits) {
    const Impl::Key key{source.name, source.text};
    std::shared_ptr<const Program> result;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->entries.find(key);
        if (it != impl_->entries.end()) return it->second;
    }
    auto program = std::make_shared<const Program>(
        compile(source.text, source.name, limits));
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto [it, inserted] = impl_->entries.emplace(key, program);
    if (inserted) {
        impl_->order.push_back(key);
        impl_->total_bytes += Impl::cost(key, *program);
        // Eviction may erase the just-inserted entry (it may exceed it on its
        // own); the local shared_ptr keeps the program alive for this render.
        impl_->evict_over_budget();
        result = program;
    } else {
        result = it->second; // another thread compiled the same source first
    }
    return result;
}

void ProgramCache::invalidate(const Source &source) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const Impl::Key key{source.name, source.text};
    auto it = impl_->entries.find(key);
    if (it == impl_->entries.end()) return;
    impl_->total_bytes -= Impl::cost(key, *it->second);
    impl_->entries.erase(it);
    for (auto o = impl_->order.begin(); o != impl_->order.end(); ++o) {
        if (*o == key) { impl_->order.erase(o); break; }
    }
}

void ProgramCache::clear() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->entries.clear();
    impl_->order.clear();
    impl_->total_bytes = 0;
}

std::size_t ProgramCache::entries() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->entries.size();
}

std::size_t ProgramCache::bytes() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->total_bytes;
}

namespace {

std::string read_bounded(const std::filesystem::path &p, std::size_t limit) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read template: " + p.string());
    std::string result;
    char chunk[8192];
    while (in.read(chunk, sizeof chunk) || in.gcount()) {
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n > limit || result.size() > limit - n)
            throw std::runtime_error("template exceeds file size limit: " + p.string());
        result.append(chunk, n);
    }
    if (in.bad()) throw std::runtime_error("template I/O error: " + p.string());
    return result;
}

bool within(const std::filesystem::path &path, const std::filesystem::path &root) {
    auto a = path.begin(), b = root.begin();
    for (; b != root.end(); ++a, ++b) {
        if (a == path.end() || *a != *b) return false;
    }
    return true;
}

// Bounded Lua allocator: per-render quota with usage/peak accounting. Each
// block is prefixed with a header so realloc/free can adjust the budget
// without trusting the caller's old-size hint. Returning nullptr makes the
// Lua VM raise its memory error, which surfaces as a QMR_EVALUATE failure.
struct BoundedAllocator {
    struct Header { std::size_t size; std::size_t padding; };
    static constexpr std::size_t kHeaderSize = sizeof(Header); // keeps max alignment

    std::size_t limit = 0;
    std::size_t used = 0;
    std::size_t peak = 0;

    void *allocate(std::size_t n) {
        if (n == 0) n = 1;
        if (used >= limit || n > limit - used) return nullptr;
        void *base = std::malloc(n + kHeaderSize);
        if (!base) return nullptr;
        static_cast<Header *>(base)->size = n;
        used += n;
        peak = std::max(peak, used);
        return static_cast<char *>(base) + kHeaderSize;
    }
    void *reallocate(void *ptr, std::size_t n) {
        if (!ptr) return allocate(n);
        if (n == 0) n = 1;
        Header *header = reinterpret_cast<Header *>(static_cast<char *>(ptr) - kHeaderSize);
        const std::size_t old = header->size;
        if (n > old && (used >= limit || n - old > limit - used)) return nullptr;
        void *base = std::realloc(header, n + kHeaderSize);
        if (!base) return nullptr;
        static_cast<Header *>(base)->size = n;
        used = used - old + n;
        peak = std::max(peak, used);
        return static_cast<char *>(base) + kHeaderSize;
    }
    void deallocate(void *ptr, std::size_t) {
        if (!ptr) return;
        Header *header = reinterpret_cast<Header *>(static_cast<char *>(ptr) - kHeaderSize);
        used -= header->size;
        std::free(header);
    }
};

// ---------------------------------------------------------------------------
// Render runtime
// ---------------------------------------------------------------------------
struct Runtime {
    const Options &options;
    std::string output;
    std::vector<std::string> stack;
    std::size_t budget;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
    std::size_t context_bytes = 0;
    int safe_html_ref = LUA_NOREF;
    lua_State *L = nullptr;

    void append(std::string_view piece) {
        const auto max = options.limits.max_output_bytes;
        if (piece.size() > max || output.size() > max - piece.size())
            throw std::runtime_error("QMR_OUTPUT_LIMIT: rendered output exceeds limit");
        output.append(piece);
    }

    void push_value(const Value &value, std::size_t depth) {
        if (depth > options.limits.max_value_depth)
            throw std::runtime_error("QMR_CONTEXT_DEPTH: model nesting exceeds limit");
        std::visit([&](const auto &arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) lua_pushnil(L);
            else if constexpr (std::is_same_v<T, bool>) lua_pushboolean(L, arg);
            else if constexpr (std::is_same_v<T, std::int64_t>) {
                if constexpr (sizeof(lua_Integer) < sizeof(std::int64_t)) {
                    if (arg > std::numeric_limits<lua_Integer>::max() ||
                        arg < std::numeric_limits<lua_Integer>::min())
                        throw std::runtime_error("QMR_CONTEXT: integer out of range");
                }
                lua_pushinteger(L, static_cast<lua_Integer>(arg));
            } else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(arg)) throw std::runtime_error("QMR_CONTEXT: non-finite model number");
                lua_pushnumber(L, static_cast<lua_Number>(arg));
            } else if constexpr (std::is_same_v<T, std::string>) {
                context_add(arg.size());
                lua_pushlstring(L, arg.data(), arg.size());
            } else if constexpr (std::is_same_v<T, SafeHtml>) {
                context_add(arg.html.size());
                push_safe_html(arg.html);
            } else if constexpr (std::is_same_v<T, Value::Array>) {
                context_add(arg.size());
                lua_createtable(L, 0, 0);
                const int table = lua_absindex(L, -1);
                for (std::size_t i = 0; i < arg.size(); ++i) {
                    if (i >= static_cast<std::size_t>(std::numeric_limits<lua_Integer>::max()))
                        throw std::runtime_error("QMR_CONTEXT: oversized array");
                    push_value(arg[i], depth + 1);
                    lua_rawseti(L, table, static_cast<lua_Integer>(i + 1));
                }
            } else if constexpr (std::is_same_v<T, Value::Object>) {
                context_add(arg.size());
                lua_createtable(L, 0, 0);
                const int table = lua_absindex(L, -1);
                for (const auto &[key, child] : arg) {
                    context_add(key.size());
                    lua_pushlstring(L, key.data(), key.size());
                    push_value(child, depth + 1);
                    lua_rawset(L, table);
                }
            }
        }, value.data);
    }
    void context_add(std::size_t count) {
        const auto max = options.limits.max_context_bytes;
        if (count > max || context_bytes > max - count)
            throw std::runtime_error("QMR_CONTEXT_LIMIT: data exceeds limit");
        context_bytes += count;
    }

    void push_safe_html(const std::string &html) {
        void *storage = lua_newuserdatauv(L, sizeof(std::string), 0);
        if (!storage) throw std::runtime_error("QMR_LUA: cannot allocate SafeHtml value");
        new (storage) std::string(html);
        lua_rawgeti(L, LUA_REGISTRYINDEX, safe_html_ref);
        lua_setmetatable(L, -2);
    }

    std::shared_ptr<const Program> compile_or_cache(const Source &source) const {
        if (options.cache) return options.cache->get(source, options.limits);
        return std::make_shared<const Program>(
            compile(source.text, source.name, options.limits));
    }

    // Raises with the Lua position mapped back to the template location when
    // possible, so diagnostics from included templates keep their origin.
    [[noreturn]] void fail(const Program &program, const Source &source,
                           const char *code, const std::string &message) const {
        static const std::regex canonical(
            R"re(^(.+):(\d+):(\d+): (QMR_[A-Z_]+): (.*)$)re", std::regex::optimize);
        std::smatch match;
        if (std::regex_search(message, match, canonical)) {
            throw Error(match[4].str(), match[1].str(),
                        static_cast<std::size_t>(std::stoull(match[2].str())),
                        static_cast<std::size_t>(std::stoull(match[3].str())),
                        match[5].str());
        }
        // Lua diagnostics: '[string "name"]:LINE:' (loaded chunk names are
        // not '@'-prefixed, so Lua brackets them) or 'name:LINE:'.
        std::size_t lua_line = 0, prefix = 0;
        auto find_bracketed = [&](const char *marker) -> bool {
            std::size_t at = message.find(marker);
            if (at == std::string::npos) return false;
            std::size_t digits = at + 2;
            lua_line = 0;
            while (digits < message.size() && message[digits] >= '0' && message[digits] <= '9') {
                lua_line = lua_line * 10 + static_cast<std::size_t>(message[digits] - '0');
                ++digits;
            }
            if (lua_line == 0 || digits >= message.size() || message[digits] != ':')
                return false;
            prefix = digits + 1;
            return true;
        };
        bool positioned = find_bracketed("]:");
        if (!positioned) {
            const std::string headed = source.name + ":";
            if (message.rfind(headed, 0) == 0) {
                std::size_t digits = headed.size();
                lua_line = 0;
                while (digits < message.size() && message[digits] >= '0' &&
                       message[digits] <= '9') {
                    lua_line = lua_line * 10 + static_cast<std::size_t>(message[digits] - '0');
                    ++digits;
                }
                if (lua_line > 0 && digits < message.size() && message[digits] == ':') {
                    positioned = true;
                    prefix = digits + 1;
                }
            }
        }
        if (positioned) {
            std::size_t line = 0, column = 0;
            if (program.locate_lua_line(lua_line, line, column))
                throw Error(code, source.name, line, column, message.substr(prefix));
        }
        throw std::runtime_error(std::string(code) + ": " + message);
    }

    void run(Source source) {
        if (stack.size() >= options.limits.max_include_depth)
            throw std::runtime_error("QMR_INCLUDE_DEPTH: maximum include depth exceeded");
        if (std::find(stack.begin(), stack.end(), source.name) != stack.end())
            throw std::runtime_error("QMR_INCLUDE_CYCLE: " + source.name);
        struct StackGuard {
            std::vector<std::string> &stack;
            explicit StackGuard(std::vector<std::string> &s, std::string name) : stack(s) {
                stack.push_back(std::move(name));
            }
            ~StackGuard() { stack.pop_back(); }
        } guard(stack, source.name);
        auto program = compile_or_cache(source);
        if (luaL_loadbufferx(L, program->lua_source.data(), program->lua_source.size(),
                             source.name.c_str(), "t") != LUA_OK) {
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua syntax error";
            lua_pop(L, 1);
            fail(*program, source, "QMR_COMPILE_LUA", error);
        }
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua execution error";
            if (error.empty()) error = "Lua execution error";
            lua_pop(L, 1);
            fail(*program, source, "QMR_EVALUATE", error);
        }
    }
};

Runtime *runtime(lua_State *L) {
    return *static_cast<Runtime **>(lua_getextraspace(L));
}

void qmr_instruction_hook(lua_State *L, lua_Debug *) {
    Runtime *state = runtime(L);
    if (!state) return;
    if (state->budget <= 100) luaL_error(L, "QMR_INSTRUCTION_LIMIT");
    else state->budget -= 100;
    if (std::chrono::steady_clock::now() > state->deadline)
        luaL_error(L, "QMR_DEADLINE: render exceeded wall-clock budget");
}

bool is_safe_html(lua_State *L, int index, Runtime &state) {
    if (lua_type(L, index) != LUA_TUSERDATA) return false;
    if (!lua_getmetatable(L, index)) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, state.safe_html_ref);
    const bool same = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return same;
}

int safe_html_gc(lua_State *L) {
    auto *text = static_cast<std::string *>(lua_touserdata(L, 1));
    if (text) text->~basic_string();
    return 0;
}

int lua_safe_html(lua_State *L) {
    std::size_t size = 0;
    const char *text = luaL_checklstring(L, 1, &size);
    Runtime *state = runtime(L);
    if (!state || state->safe_html_ref == LUA_NOREF)
        luaL_error(L, "QMR_SAFE_HTML: no runtime registered");
    void *storage = lua_newuserdatauv(L, sizeof(std::string), 0);
    if (!storage) luaL_error(L, "QMR_SAFE_HTML: out of memory");
    new (storage) std::string(text, size);
    lua_rawgeti(L, LUA_REGISTRYINDEX, state->safe_html_ref);
    lua_setmetatable(L, -2);
    return 1;
}

// Native renderer callbacks return (ok, error), never raise a Lua longjmp
// across a C++ stack containing nontrivial values. Generated Lua raises after
// the callback has returned and its C++ destructors have run.
int native_write(lua_State *L) {
    try {
        if (lua_type(L, 1) != LUA_TSTRING) throw std::runtime_error("QMR_WRITE: expected string");
        std::size_t n = 0;
        const char *s = lua_tolstring(L, 1, &n);
        runtime(L)->append(std::string_view(s, n));
        lua_pushboolean(L, 1);
        lua_pushnil(L);
        return 2;
    } catch (const std::exception &e) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, e.what());
        return 2;
    }
}

int native_emit(lua_State *L) {
    try {
        Runtime &state = *runtime(L);
        const bool do_escape = lua_toboolean(L, 2) != 0;
        // Capture the type up front: lua_tolstring below converts the stack
        // slot in place, so re-reading lua_type would misclassify numbers.
        const int type = lua_type(L, 1);
        std::string value;
        bool trusted_html = false;
        switch (type) {
        case LUA_TNIL: break;
        case LUA_TBOOLEAN: value = lua_toboolean(L, 1) ? "true" : "false"; break;
        case LUA_TSTRING:
        case LUA_TNUMBER: {
            std::size_t n = 0;
            const char *s = lua_tolstring(L, 1, &n);
            value.assign(s, n);
            break;
        }
        case LUA_TUSERDATA: {
            if (!is_safe_html(L, 1, state))
                throw std::runtime_error(
                    "QMR_TYPE: interpolation requires nil, bool, number, string or trusted SafeHtml");
            value = *static_cast<std::string *>(lua_touserdata(L, 1));
            trusted_html = true;
            break;
        }
        default:
            throw std::runtime_error(
                "QMR_TYPE: interpolation requires nil, bool, number, string or trusted SafeHtml");
        }
        // Context-sensitive escaping for {{ ... }}; {{{ ... }}} is always raw.
        switch (state.options.codec) {
        case Codec::Html:
            if (do_escape && !trusted_html) value = escape_html(value);
            break;
        case Codec::Plain:
            break; // no escaping in plain-text mode
        case Codec::Xml:
            if (do_escape) value = escape_xml(value);
            break; // SafeHtml markup is not XML-trusted and stays escaped
        case Codec::Json:
            if (do_escape) {
                if (type == LUA_TNIL) value = "null";
                else if (type == LUA_TSTRING || trusted_html)
                    value = escape_json(value); // full quoted string literal
                // numbers and booleans are already valid JSON scalars
            }
            break;
        }
        state.append(value);
        lua_pushboolean(L, 1);
        lua_pushnil(L);
        return 2;
    } catch (const std::exception &e) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, e.what());
        return 2;
    }
}

int native_include(lua_State *L) {
    int saved_vars = LUA_NOREF;
    auto restore = [&]() {
        if (saved_vars != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, saved_vars);
            lua_setglobal(L, "vars");
            luaL_unref(L, LUA_REGISTRYINDEX, saved_vars);
            saved_vars = LUA_NOREF;
        }
    };
    try {
        if (lua_type(L, 1) != LUA_TSTRING)
            throw std::runtime_error("QMR_INCLUDE: path must be a string");
        if (!lua_isnoneornil(L, 2) && !lua_istable(L, 2))
            throw std::runtime_error("QMR_INCLUDE: optional variables must be a table");
        std::size_t size = 0;
        const char *text = lua_tolstring(L, 1, &size);
        Runtime &r = *runtime(L);
        if (!r.options.resolver) throw std::runtime_error("QMR_INCLUDE: no include resolver configured");
        const auto name = std::string_view(text, size);
        const auto from = std::string_view(r.stack.back());
        auto source = r.options.resolver(name, from);
        if (!source) throw std::runtime_error("QMR_INCLUDE_NOT_FOUND: " + std::string(name));
        lua_getglobal(L, "vars");
        saved_vars = luaL_ref(L, LUA_REGISTRYINDEX);
        if (lua_istable(L, 2)) lua_pushvalue(L, 2);
        else lua_pushnil(L);
        lua_setglobal(L, "vars");
        r.run(std::move(*source));
        restore();
        lua_pushboolean(L, 1);
        lua_pushnil(L);
        return 2;
    } catch (const std::exception &e) {
        restore();
        lua_pushboolean(L, 0);
        lua_pushstring(L, e.what());
        return 2;
    }
}

int lua_escape_html(lua_State *L) {
    std::size_t n = 0;
    if (lua_type(L, 1) != LUA_TSTRING) { lua_pushnil(L); return 1; }
    const char *v = lua_tolstring(L, 1, &n);
    auto encoded = escape_html(std::string_view(v, n));
    lua_pushlstring(L, encoded.data(), encoded.size());
    return 1;
}
int lua_escape_url(lua_State *L) {
    std::size_t n = 0;
    if (lua_type(L, 1) != LUA_TSTRING) { lua_pushnil(L); return 1; }
    const char *v = lua_tolstring(L, 1, &n);
    auto encoded = escape_url(std::string_view(v, n));
    lua_pushlstring(L, encoded.data(), encoded.size());
    return 1;
}
int lua_default(lua_State *L) {
    const int type = lua_type(L, 1);
    bool missing = type == LUA_TNIL || (type == LUA_TBOOLEAN && !lua_toboolean(L, 1));
    if (type == LUA_TSTRING) {
        std::size_t n = 0;
        (void)lua_tolstring(L, 1, &n);
        missing = n == 0;
    }
    lua_pushvalue(L, missing ? 2 : 1);
    return 1;
}

void set_c_function(lua_State *L, const char *name, lua_CFunction func) {
    lua_pushcfunction(L, func);
    lua_setglobal(L, name);
}

} // namespace

Resolver filesystem_resolver(std::filesystem::path root, std::size_t max_source_bytes) {
    namespace fs = std::filesystem;
    root = fs::canonical(root);
    if (!fs::is_directory(root)) throw std::runtime_error("QMR_ROOT: root is not a directory");
    return [root = std::move(root), max_source_bytes](std::string_view request,
                                                      std::string_view including) -> std::optional<Source> {
        if (request.empty() || request.size() > 4096 || request.find('\0') != std::string_view::npos)
            return std::nullopt;
        fs::path relative = fs::path(std::string(request));
        if (relative.is_absolute() || relative.has_root_name()) return std::nullopt;
        for (const auto &part : relative)
            if (part == "..") return std::nullopt;
        fs::path parent = fs::path(std::string(including)).parent_path();
        if (parent.empty()) parent = root;
        std::error_code ec;
        auto candidate = fs::canonical(parent / relative, ec);
        if (ec || !within(candidate, root) || !fs::is_regular_file(candidate, ec) || ec)
            return std::nullopt;
        return Source{candidate.string(), read_bounded(candidate, max_source_bytes)};
    };
}

Engine::Engine(Options options) : options_(std::move(options)) {}

std::string Engine::render(std::string_view source, const Context &data, std::string name) const {
    // A fresh state per render avoids data leakage and avoids sharing Lua VMs across threads.
    if (options_.limits.max_instructions < 100 || options_.limits.max_include_depth == 0)
        throw std::runtime_error("QMR_LIMIT: invalid runtime limit");
    Runtime render_state{options_, {}, {}, options_.limits.max_instructions};
    if (options_.limits.max_duration > std::chrono::milliseconds::zero())
        render_state.deadline = std::chrono::steady_clock::now() +
                                options_.limits.max_duration;
    auto allocator = std::make_shared<BoundedAllocator>(options_.limits.max_memory_bytes);
    kaguya::State state(kaguya::LoadLibs{{"_G", luaopen_base},
                                         {"table", luaopen_table},
                                         {"string", luaopen_string},
                                         {"math", luaopen_math},
                                         {"utf8", luaopen_utf8}},
                        allocator);
    lua_State *L = state.state();
    if (!L) throw std::runtime_error("QMR_LUA: failed to create Lua VM");
    render_state.L = L;
    // Deliberate subset, not a security boundary for hostile Lua templates.
    for (auto *key : {"dofile", "loadfile", "load", "require", "collectgarbage", "print",
                      "rawset", "setmetatable", "getmetatable"}) {
        lua_pushnil(L);
        lua_setglobal(L, key);
    }
    lua_pushnil(L); lua_setglobal(L, "io");
    lua_pushnil(L); lua_setglobal(L, "os");
    lua_pushnil(L); lua_setglobal(L, "package");
    lua_pushnil(L); lua_setglobal(L, "debug");
    *static_cast<Runtime **>(lua_getextraspace(L)) = &render_state;
    lua_sethook(L, qmr_instruction_hook, LUA_MASKCOUNT, 100);
    set_c_function(L, "__qmr_emit_native", native_emit);
    set_c_function(L, "__qmr_write_native", native_write);
    set_c_function(L, "__qmr_include_native", native_include);
    // Trusted SafeHtml metatable; the registry ref is the comparison token.
    lua_newtable(L);
    lua_pushcfunction(L, safe_html_gc); lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "qmr.safe_html"); lua_setfield(L, -2, "__name");
    lua_pushcfunction(L, lua_safe_html); lua_setfield(L, -2, "wrap");
    render_state.safe_html_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    lua_pushliteral(L, "0.2"); lua_setfield(L, -2, "version");
    lua_pushcfunction(L, lua_escape_html); lua_setfield(L, -2, "escape_html");
    lua_pushcfunction(L, lua_escape_url); lua_setfield(L, -2, "escape_url");
    lua_pushcfunction(L, lua_default); lua_setfield(L, -2, "default");
    lua_pushcfunction(L, lua_safe_html); lua_setfield(L, -2, "safe_html");
    lua_setglobal(L, "qmr");
    Value root(data);
    render_state.push_value(root, 0);
    lua_setglobal(L, "data");
    const std::string input(source);
    // The Lua VM is owned by `state`; the allocator outlives lua_close because
    // the kaguya State holds the shared_ptr. render_state is destroyed after
    // the VM closes, and the SafeHtml __gc only touches its own userdata.
    render_state.run(Source{std::move(name), input});
    return render_state.output;
}

std::string Engine::render_file(const std::filesystem::path &file, const Context &data,
                                const std::filesystem::path &root) const {
    namespace fs = std::filesystem;
    const auto real_file = fs::canonical(file);
    const auto real_root = fs::canonical(root.empty() ? real_file.parent_path() : root);
    if (!within(real_file, real_root)) throw std::runtime_error("QMR_ROOT: template outside root");
    Options file_options = options_;
    if (!file_options.resolver)
        file_options.resolver = filesystem_resolver(real_root, options_.limits.max_template_bytes);
    return Engine(std::move(file_options)).render(
        read_bounded(real_file, options_.limits.max_template_bytes), data, real_file.string());
}

} // namespace moonstone::qmr
