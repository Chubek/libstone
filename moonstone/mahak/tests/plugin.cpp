#include "mahak/Engine.hpp"
#include "mahak/Execution.hpp"
#include "mahak/Plugin.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

using namespace moonstone::mahak;
namespace fs = std::filesystem;

namespace {
std::string find_tool(std::string_view name) {
    const char *env = std::getenv("PATH");
    if (!env)
        return {};
    std::string_view path(env);
    while (true) {
        const auto end = path.find(':');
        const auto candidate = fs::absolute(fs::path(path.substr(0, end)) / name);
        if (::access(candidate.c_str(), X_OK) == 0 && fs::is_regular_file(candidate))
            return candidate.string();
        if (end == std::string_view::npos)
            return {};
        path.remove_prefix(end + 1);
    }
}

struct TempDir {
    fs::path path;
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::string read_file(const fs::path &path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string compile(const std::string &cc, const fs::path &root, const char *name,
                    const char *define = nullptr) {
    const auto output = (root / (std::string(name) + ".so")).string();
    CommandPlan command;
    command.argv = {cc, "-shared", "-fPIC", "-std=c++20", "-Wall", "-Wextra", "-Wpedantic",
                    "-Werror", "-I" MAHAK_PLUGIN_INCLUDE, "-x", "c++", (root / "oracle.cpp").string(),
                    "-L" MAHAK_SDK_DIR, "-Wl,-rpath," MAHAK_SDK_DIR, "-lmahaksdk", "-lstdc++",
                    "-o", output};
    if (define)
        command.argv.insert(command.argv.begin() + 1, define);
    ProcessOptions options;
    options.timeout_ms = 15000;
    auto result = run_command(command, options);
    if (!result || result.value().validity != Validity::Valid) {
        std::cerr << "plugin compilation failed: "
                  << (result ? result.value().stderr_text : result.error().message) << '\n';
        return {};
    }
    return output;
}

void expect_rejected(const Result<void> &result, std::string_view reason) {
    assert(!result);
    if (result.error().message.find(reason) == std::string::npos) {
        std::cerr << "expected " << reason << ", got: " << result.error().message << '\n';
        assert(false);
    }
}
} // namespace

int main() {
    const auto cc = find_tool("cc");
    if (cc.empty() || find_tool("ld").empty()) {
        std::cout << "SKIP: cc and ld are required\n";
        return 0;
    }
    if (::access("/bin/echo", X_OK) != 0) {
        std::cout << "SKIP: /bin/echo is required\n";
        return 0;
    }
    auto pattern = (fs::temp_directory_path() / "mahak-plugin-XXXXXX").string();
    assert(::mkdtemp(pattern.data()));
    const TempDir temp{pattern};
    fs::copy_file(MAHAK_PLUGIN_FIXTURE, temp.path / "oracle.cpp");
    const auto good = compile(cc, temp.path, "good");
    const auto bad_abi = compile(cc, temp.path, "bad-abi", "-DBAD_ABI");
    const auto no_entry = compile(cc, temp.path, "no-entry", "-DMISSING_ENTRY");
    const auto bad_size = compile(cc, temp.path, "bad-size", "-DBAD_SIZE");
    const auto no_hook = compile(cc, temp.path, "no-hook", "-DMISSING_HOOK");
    const auto bad_verdict = compile(cc, temp.path, "bad-verdict", "-DBAD_VERDICT");
    assert(!good.empty() && !bad_abi.empty() && !no_entry.empty() && !bad_size.empty() &&
           !no_hook.empty() && !bad_verdict.empty());
    const auto duplicate = temp.path / "duplicate.so";
    fs::copy_file(good, duplicate);
    const auto log = temp.path / "teardown.log";
    assert(::setenv("MAHAK_PLUGIN_TEARDOWN_LOG", log.c_str(), 1) == 0);

    {
        auto registry = PluginRegistry::create();
        assert(registry);
        auto &plugins = registry.value();
        expect_rejected(plugins.load(bad_abi), "ABI version");
        assert(plugins.list().empty()); // Even the first, valid entry was not published.
        assert(read_file(log) == "teardown\n");
        fs::remove(log);
        expect_rejected(plugins.load(no_entry), "mahak_plugin_init_v1");
        expect_rejected(plugins.load(bad_size), "hook size");
        expect_rejected(plugins.load(no_hook), "mandatory hook");
        expect_rejected(plugins.load(bad_abi, {.max_plugins = 1}), "count");
        assert(plugins.list().empty());
        assert(plugins.load(good));
        assert(plugins.hooks("fixture.oracle") && plugins.info("fixture.oracle"));
        expect_rejected(plugins.load(duplicate.string()), "duplicate plugin id");
        assert(plugins.list().size() == 1);
        fs::remove(log);
        plugins.teardown();
        plugins.teardown();
        assert(plugins.list().empty() && !plugins.hooks("fixture.oracle"));
        assert(read_file(log) == "teardown\n");
        assert(plugins.load(good));
    }
    assert(read_file(log) == "teardown\nteardown\n"); // Reused instance cleaned by destructor.
    fs::remove(log);

    const auto manifest = (temp.path / "suite.json").string();
    std::ofstream(manifest) << R"({
      "schema": 1, "id": "test-plugin", "version": "1.0.0",
      "targets": [{"id": "echo", "kind": "executable",
                   "execute": {"argv": ["/bin/echo", "plugin stdout"]}}],
      "benchmarks": [{"id": "judge", "modes": ["execute-only"],
                      "oracle": {"plugin": "fixture.oracle", "expected_exit": 99}}]
    })";
    EngineOptions options;
    options.plugins = {good};
    {
        Engine engine(options);
        for (int i = 0; i < 2; ++i) {
            auto session = engine.run_file(manifest);
            if (!session)
                std::cerr << session.error().message << '\n';
            else if (session.value().status != "completed") {
                for (const auto &note : session.value().notes)
                    std::cerr << note << '\n';
                for (const auto &run : session.value().runs)
                    std::cerr << run.status << ": " << run.message << '\n';
            }
            assert(session && session.value().status == "completed");
            assert(session.value().runs.size() == 1);
            const auto &oracle = session.value().runs.front().oracle;
            assert(oracle.passed && oracle.validity == Validity::Valid &&
                   oracle.message == "fixture judged stdout");
        }
        assert(!fs::exists(log));
    }
    assert(read_file(log) == "teardown\n"); // One load across both runs.
    options.plugins = {bad_verdict};
    Engine engine(options);
    auto session = engine.run_file(manifest);
    assert(session && session.value().status == "failed" && session.value().runs.empty());
    assert(session.value().notes.size() == 1 &&
           session.value().notes.front().find("inconsistent or default verdict") != std::string::npos);
    std::cout << "native plugin tests passed\n";
}
