#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace asdl {

struct Schema;

struct LuaOptions {
    std::map<std::string, std::string> values;
    std::vector<std::string> arguments;
    std::size_t memory_limit = 64 * 1024 * 1024;
    std::size_t instruction_limit = 2'000'000;
    std::size_t traversal_limit = 256;
};

// Owns one VM and its typed AST arena. A session is not thread-safe.
class LuaSession {
public:
    explicit LuaSession(const Schema& schema, LuaOptions options = {});
    ~LuaSession();
    LuaSession(const LuaSession&) = delete;
    LuaSession& operator=(const LuaSession&) = delete;

    lua_State* state() const noexcept;
    // Text chunks only. All entry points preserve the caller's Lua stack.
    // Syntax errors, runtime errors and exhausted budgets throw runtime_error.
    void check(std::string_view source, std::string_view chunk = "=(asdl)");
    void run(std::string_view source, std::string_view chunk = "=(asdl)");
    // Native installation entry point: arguments are the asdl table and context.
    void extend(int (*install)(lua_State*), void* context = nullptr);
    std::size_t diagnostic_count(std::string_view severity = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace asdl
