#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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
};

enum class EscapeMode { Html, None };

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
struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;
    Storage data = nullptr;
    Value() = default;
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool v) : data(v) {}
    Value(int v) : data(static_cast<std::int64_t>(v)) {}
    Value(std::int64_t v) : data(v) {}
    Value(double v) : data(v) {}
    Value(const char *v) : data(std::string(v)) {}
    Value(std::string v) : data(std::move(v)) {}
    Value(Array v) : data(std::move(v)) {}
    Value(Object v) : data(std::move(v)) {}
};
using Context = Value::Object;

struct Program {
    std::string name;
    std::string lua_source;
    std::size_t nodes = 0;
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

struct Options {
    Limits limits;
    EscapeMode escape = EscapeMode::Html;
    Resolver resolver;
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

std::string escape_html(std::string_view text);
std::string escape_url(std::string_view text);

} // namespace moonstone::qmr
