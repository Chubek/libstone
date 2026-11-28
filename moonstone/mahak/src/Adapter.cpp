#include "mahak/Adapter.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "mahak/Execution.hpp"

namespace moonstone::mahak {
inline namespace v1 {
namespace {

Error adapter_error(std::string_view context, std::string_view message,
                    ErrorCategory category = ErrorCategory::InvalidInput) {
    Error error;
    error.category = category;
    error.message = std::string(context) + ": " + std::string(message);
    return error;
}

std::vector<std::string> split_path(std::string_view value) {
    std::vector<std::string> entries;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find(':', begin);
        const std::size_t stop = end == std::string_view::npos ? value.size() : end;
        entries.emplace_back(value.substr(begin, stop - begin));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return entries;
}

Result<void> require_stages(const TargetSpec &target, std::string_view adapter) {
    if (target.stages.empty() && !target.execute) {
        return adapter_error(adapter, "target '" + target.id +
                                          "' declares neither stages nor an execute command");
    }
    for (const auto &stage : target.stages) {
        if (stage.commands.empty())
            return adapter_error(adapter, "target '" + target.id + "' has an empty " +
                                              std::string(name(stage.kind)) + " stage");
        for (const auto &command : stage.commands) {
            if (command.argv.empty())
                return adapter_error(adapter, "target '" + target.id +
                                                  "' has a command with an empty argv");
        }
    }
    return {};
}

} // namespace

AdapterInfo GenericCommandAdapter::info() const {
    AdapterInfo result;
    result.id = "generic-command";
    result.kind = TargetKind::CustomCommand;
    result.capabilities = AdapterCapability::Compile | AdapterCapability::Link |
                          AdapterCapability::Execute | AdapterCapability::Startup |
                          AdapterCapability::Instrumentation;
    result.description = "Manifest-driven commands executed without a shell";
    return result;
}

Result<std::vector<StageSpec>> GenericCommandAdapter::prepare(const TargetSpec &target) const {
    if (target.kind != TargetKind::CustomCommand && target.kind != TargetKind::Executable &&
        target.kind != TargetKind::Interpreter && target.kind != TargetKind::VirtualMachine &&
        target.kind != TargetKind::Library && target.kind != TargetKind::Compiler) {
        return adapter_error("generic-command",
                             "unsupported target kind for target '" + target.id + "'");
    }
    if (auto checked = require_stages(target, "generic-command"); !checked)
        return checked.error();
    return target.stages;
}

AdapterInfo NativeCompilerAdapter::info() const {
    AdapterInfo result;
    result.id = "native-compiler";
    result.kind = TargetKind::Compiler;
    result.capabilities = AdapterCapability::Compile | AdapterCapability::Link |
                          AdapterCapability::Execute | AdapterCapability::Instrumentation;
    result.description = "C/C++ compiler driver stages";
    return result;
}

Result<std::vector<StageSpec>> NativeCompilerAdapter::prepare(const TargetSpec &target) const {
    if (target.kind != TargetKind::Compiler)
        return adapter_error("native-compiler",
                             "target '" + target.id + "' is not a compiler target");
    if (target.stages.empty())
        return adapter_error("native-compiler",
                             "compiler target '" + target.id + "' declares no compile stages");
    if (auto checked = require_stages(target, "native-compiler"); !checked)
        return checked.error();
    return target.stages;
}

AdapterInfo NativeExecutableAdapter::info() const {
    AdapterInfo result;
    result.id = "native-executable";
    result.kind = TargetKind::Executable;
    result.capabilities = AdapterCapability::Execute | AdapterCapability::Startup;
    result.description = "Pre-built native executables";
    return result;
}

Result<std::vector<StageSpec>> NativeExecutableAdapter::prepare(const TargetSpec &target) const {
    if (target.kind != TargetKind::Executable)
        return adapter_error("native-executable",
                             "target '" + target.id + "' is not an executable target");
    if (!target.execute)
        return adapter_error("native-executable",
                             "executable target '" + target.id + "' has no execute command");
    if (target.execute->argv.empty())
        return adapter_error("native-executable",
                             "executable target '" + target.id + "' has an empty argv");
    return target.stages;
}

AdapterRegistry::AdapterRegistry() {
    add(std::make_shared<GenericCommandAdapter>());
    add(std::make_shared<NativeCompilerAdapter>());
    add(std::make_shared<NativeExecutableAdapter>());
}

void AdapterRegistry::add(std::shared_ptr<const Adapter> adapter) {
    if (!adapter)
        return;
    for (const auto &existing : adapters_) {
        if (existing->info().id == adapter->info().id)
            return;
    }
    adapters_.push_back(std::move(adapter));
}

const Adapter *AdapterRegistry::find(std::string_view id) const {
    for (const auto &adapter : adapters_) {
        if (adapter->info().id == id)
            return adapter.get();
    }
    return nullptr;
}

const Adapter *AdapterRegistry::find_for(TargetKind kind) const {
    for (const auto &adapter : adapters_) {
        if (adapter->info().kind == kind)
            return adapter.get();
    }
    return nullptr;
}

std::vector<AdapterInfo> AdapterRegistry::list() const {
    std::vector<AdapterInfo> listed;
    listed.reserve(adapters_.size());
    for (const auto &adapter : adapters_)
        listed.push_back(adapter->info());
    return listed;
}

Result<std::string> find_executable(std::string_view name) {
    if (name.empty())
        return adapter_error("find-executable", "empty executable name");
    if (name.find('/') != std::string_view::npos) {
        const std::string candidate(name);
        struct stat status {};
        if (::stat(candidate.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
            ::access(candidate.c_str(), X_OK) != 0) {
            return adapter_error("find-executable", "not executable: " + candidate,
                                 ErrorCategory::NotFound);
        }
        return candidate;
    }
    const char *path_environment = std::getenv("PATH");
    const std::string path = path_environment != nullptr ? path_environment
                                                         : "/usr/local/bin:/usr/bin:/bin";
    for (const auto &entry : split_path(path)) {
        const std::string directory = entry.empty() ? "." : entry;
        const std::string candidate = directory + "/" + std::string(name);
        struct stat status {};
        if (::stat(candidate.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
            continue;
        if (::access(candidate.c_str(), X_OK) != 0)
            continue;
        return candidate;
    }
    return adapter_error("find-executable", "'" + std::string(name) + "' not found in PATH",
                         ErrorCategory::NotFound);
}

ToolProbe probe_tool(std::string_view name) {
    ToolProbe probe;
    probe.name = std::string(name);
    auto located = find_executable(name);
    if (!located) {
        probe.diagnostics = located.error().message;
        return probe;
    }
    probe.path = located.value();
    CommandPlan command;
    command.argv = {probe.path, "--version"};
    ProcessOptions options;
    options.timeout_ms = 5000;
    options.max_output_bytes = 64u * 1024u;
    auto result = run_command(command, options);
    if (!result) {
        probe.diagnostics = result.error().message;
        return probe;
    }
    const CommandResult &executed = result.value();
    if (executed.spawn_failed) {
        probe.diagnostics = "spawn failed: " + executed.message;
        return probe;
    }
    const std::string &output =
        !executed.stdout_text.empty() ? executed.stdout_text : executed.stderr_text;
    const std::size_t newline = output.find('\n');
    probe.version = output.substr(0, newline == std::string::npos ? output.size() : newline);
    if (probe.version.empty())
        probe.version = executed.stderr_text.substr(0, executed.stderr_text.find('\n'));
    if (executed.exit_code != 0 && probe.version.empty()) {
        probe.diagnostics = "version probe exited with status " + std::to_string(executed.exit_code);
        return probe;
    }
    probe.available = true;
    return probe;
}

} // namespace v1
} // namespace moonstone::mahak
