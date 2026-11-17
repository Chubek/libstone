#pragma once

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace moonstone::qmr {

struct Limits {
    std::size_t max_template_bytes = 1024 * 1024;
    std::size_t max_compiled_bytes = 4 * 1024 * 1024;
    std::size_t max_output_bytes = 4 * 1024 * 1024;
    std::size_t max_context_bytes = 2 * 1024 * 1024;
    std::size_t max_nodes = 100000;
    std::size_t max_include_depth = 16;
    std::size_t max_instructions = 2'000'000;
    std::size_t max_value_depth = 32;
    // Per-render Lua VM allocation quota enforced by a custom allocator.
    std::size_t max_memory_bytes = 64 * 1024 * 1024;
    // Wall-clock render budget checked by the VM instruction hook.
    // Non-positive disables the deadline check.
    std::chrono::milliseconds max_duration{5000};
};

// Autoescape codec applied to {{ ... }} interpolation. {{{ ... }}} is never
// escaped. Each codec documents its own context-sensitive rules; none of them
// is a full output-format validator.
enum class Codec { Html, Plain, Json, Xml };

class Error : public std::runtime_error {
public:
    Error(std::string code, std::string file, std::size_t line, std::size_t column,
          std::string message);
    const std::string &code() const noexcept { return code_; }
    const std::string &file() const noexcept { return file_; }
    std::size_t line() const noexcept { return line_; }
    std::size_t column() const noexcept { return column_; }
private:
    std::string code_, file_;
    std::size_t line_, column_;
};

// Explicit model: strings remain strings; numeric values are Lua numbers.
// No implicit execution of model fields or user-provided Lua source.
// SafeHtml marks a string as trusted, pre-escaped markup: it is emitted
// without further escaping even through {{ ... }}. It must be applied by the
// host (or by qmr.safe_html() in a trusted template) after independent
// validation; plain strings stay escaped by default.
struct SafeHtml {
    std::string html;
};

struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                                 SafeHtml, Array, Object>;
    Storage data = nullptr;
    Value() = default;
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool v) : data(v) {}
    Value(int v) : data(static_cast<std::int64_t>(v)) {}
    Value(std::int64_t v) : data(v) {}
    Value(double v) : data(v) {}
    Value(const char *v) : data(std::string(v)) {}
    Value(std::string v) : data(std::move(v)) {}
    Value(SafeHtml v) : data(std::move(v)) {}
    Value(Array v) : data(std::move(v)) {}
    Value(Object v) : data(std::move(v)) {}
};
using Context = Value::Object;

// Origin of one generated Lua line: template line/column (1-based).
struct LineOrigin {
    std::size_t line = 1;
    std::size_t column = 1;
    bool operator==(const LineOrigin &) const = default;
};

struct Program {
    std::string name;
    std::string lua_source;
    std::size_t nodes = 0;
    // Source map: line_map[i] is the origin of Lua line (lua_header_lines+1+i).
    std::size_t lua_header_lines = 0;
    std::vector<LineOrigin> line_map;

    // Maps a 1-based Lua line of lua_source back to the template position.
    bool locate_lua_line(std::size_t lua_line,
                         std::size_t &template_line, std::size_t &template_column) const {
        if (lua_line == 0 || lua_line <= lua_header_lines)
            return false;
        const std::size_t index = lua_line - lua_header_lines - 1;
        if (index >= line_map.size()) return false;
        template_line = line_map[index].line;
        template_column = line_map[index].column;
        return true;
    }
};

// Compiles an ordinary text template into text-only Lua code; NEVER loads Lua bytecode.
Program compile(std::string_view source, std::string name = "<template>",
                const Limits &limits = {});

// Resolver returns the included template's source and its *canonical identity*.
// A missing result rejects the include rather than silently producing empty output.
struct Source {
    std::string name;
    std::string text;
};
using Resolver = std::function<std::optional<Source>(std::string_view include_name,
                                                    std::string_view including_template)>;

// In-memory cache of compiled programs. Identity is the exact (name, text)
// pair, so any source change is a different entry; entries are dropped when
// the byte budget is exceeded (oldest first). Thread-safe: one cache can be
// shared by concurrently rendering engines.
class ProgramCache {
public:
    explicit ProgramCache(std::size_t max_bytes = 16 * 1024 * 1024);
    ProgramCache(const ProgramCache &) = delete;
    ProgramCache &operator=(const ProgramCache &) = delete;
    // Returns the cached program for (source.name, source.text), compiling it
    // on first use. Throws compile errors unchanged.
    std::shared_ptr<const Program> get(const Source &source, const Limits &limits);
    void invalidate(const Source &source);
    void clear();
    std::size_t entries() const;
    std::size_t bytes() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

struct Options {
    Limits limits;
    Codec codec = Codec::Html;
    Resolver resolver;
    std::shared_ptr<ProgramCache> cache;
};

class Engine {
public:
    explicit Engine(Options options = {});
    std::string render(std::string_view source, const Context &data = {},
                       std::string name = "<template>") const;
    std::string render_file(const std::filesystem::path &file, const Context &data = {},
                            const std::filesystem::path &root = {}) const;
    const Options &options() const noexcept { return options_; }
private:
    Options options_;
};

// Reads files with an upper bound. Rejects absolute include names, traversal,
// symlink escapes and cycles; files must be under canonical root.
Resolver filesystem_resolver(std::filesystem::path root, std::size_t max_source_bytes);

// Codec-specific escapers, used for {{ ... }} interpolation.
// HTML: & < > " ' become entities (defense against tag/attribute injection).
std::string escape_html(std::string_view text);
// XML text and attribute safe: & < > " ' (valid XML predefined entities).
// Does not validate XML 1.0 character legality of other control characters.
std::string escape_xml(std::string_view text);
// JSON string literal safe: " \ and C0 controls (short escapes or \u00XX).
// Non-ASCII UTF-8 passes through unchanged.
std::string escape_json(std::string_view text);
// URL query-component percent-encoding (RFC 3986 unreserved kept).
std::string escape_url(std::string_view text);

} // namespace moonstone::qmr
