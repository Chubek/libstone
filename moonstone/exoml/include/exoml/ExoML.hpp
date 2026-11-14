#pragma once
#if __has_include("../../../core/SafetyEffects.hpp")
#include "../../../core/SafetyEffects.hpp"
#else
#include <moonstone/core/SafetyEffects.hpp>
#endif
#include "Value.hpp"
#include <thread>

namespace moonstone::exoml {
bool valid_identifier(std::string_view name) noexcept;
bool valid_service_name(std::string_view name) noexcept;

struct Command {
    std::string binding, operation, function;
    std::vector<std::string> arguments;
    SourceSpan source;
};
struct Module {
    std::vector<Command> commands;
};
struct ParseLimits {
    std::size_t input_bytes = 1024 * 1024;
    std::size_t commands = 4096;
    std::size_t arguments = 128;
};
class ParseError : public std::invalid_argument {
  public:
    SourceSpan source;
    ParseError(std::string message, SourceSpan span)
        : std::invalid_argument(std::move(message)), source(std::move(span)) {}
};
class Parser {
    std::string_view input_;
    std::string file_;
    ParseLimits limits_;

  public:
    explicit Parser(std::string_view input, std::string file = {}, ParseLimits limits = {})
        : input_(input), file_(std::move(file)), limits_(limits) {}
    Module parse() const;
};

struct ServiceInfo {
    std::vector<std::string> parameter_types;
    std::string return_type;
    std::optional<std::size_t> arity;
    OwnershipContract ownership;
    bool thread_affine = false;
    std::string documentation;
};
struct ExecutionOptions {
    bool stop_on_error = false;
    std::size_t max_commands = 4096;
};

// Synchronous, caller-thread execution. External synchronization is required.
class Interpreter {
  public:
    using HostFunction = std::function<Value(const std::vector<Value> &)>;
    struct Registered {
        HostFunction callback;
        bool privileged = false;
        ServiceInfo info;
        std::thread::id registration_thread = std::this_thread::get_id();
    };
    using Services = std::map<std::string, Registered>;

  private:
    std::map<std::string, Value> symbols_;
    std::set<std::string> constants_;
    Services services_;
    std::set<std::string> permitted_;
    bool allow_hatch_ = false;
    std::optional<Value> last_;

  public:
    void register_service(std::string name, HostFunction fn, bool privileged = false);
    // All-or-nothing, including collisions with existing services.
    void register_services(Services services);
    void allow_service(std::string name);
    void revoke_service(const std::string &name) { permitted_.erase(name); }
    void enable_hatch(bool enable) { allow_hatch_ = enable; }
    const std::map<std::string, Value> &symbols() const noexcept { return symbols_; }
    std::map<std::string, Value> snapshot() const { return symbols_; }
    const Services &services() const noexcept { return services_; }
    void define(std::string name, Value value, bool constant = true);
    void clear_symbols() {
        symbols_.clear();
        constants_.clear();
        last_.reset();
    }
    Value invoke(const std::string &name, const std::vector<Value> &arguments,
                 std::string_view operation = "~get!") const;
    Diagnostics execute(const Module &module, ExecutionOptions options = {});
};
} // namespace moonstone::exoml
