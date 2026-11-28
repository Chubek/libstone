// End-to-end engine and CLI contracts. Uses /bin/echo so the test needs no compiler.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "mahak/Cli.hpp"
#include "mahak/Engine.hpp"
#include "mahak/Report.hpp"
#include "mahak/Storage.hpp"

using namespace moonstone::mahak;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

std::string write_manifest(const std::filesystem::path &directory, const std::string &contents,
                           std::string_view name) {
    const std::filesystem::path path = directory / name;
    std::ofstream stream(path);
    stream << contents;
    return path.string();
}

int run_cli(std::vector<std::string> arguments) {
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>("mahak"));
    for (auto &argument : arguments)
        argv.push_back(argument.data());
    return cli_main(static_cast<int>(argv.size()), argv.data());
}

const char *kWorkingManifest = R"({
  "schema": 1,
  "id": "engine-unit",
  "version": "1.0.0",
  "targets": [
    {"id": "echo", "kind": "executable",
     "execute": {"argv": ["/bin/echo", "hello world"]}}
  ],
  "benchmarks": [
    {"id": "greet", "modes": ["execute-only"],
     "oracle": {"kind": "stdout-contains", "expected": "hello world"},
     "sampling": {"warmup": 1, "repetitions": 3},
     "metrics": [{"name": "wall", "unit": "ns", "direction": "lower", "primary": true}]}
  ]
})";

} // namespace

int main() {
    if (!std::filesystem::exists("/bin/echo")) {
        std::cout << "SKIP: /bin/echo is required\n";
        return 0;
    }
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("mahak-engine-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const std::string manifest = write_manifest(root, kWorkingManifest, "working.json");
    const std::string invalid_manifest =
        write_manifest(root, R"({"schema":1,"id":"bad","unknown":true,"targets":[],"benchmarks":[]})",
                       "invalid.json");

    EngineOptions engine_options;
    engine_options.work_root = (root / "work").string();
    Engine engine(engine_options);
    auto environment = engine.environment();
    check(static_cast<bool>(environment), "environment probes");
    if (environment)
        check(!environment.value().architecture.empty(), "architecture is recorded");

    auto session = engine.run_file(manifest);
    check(static_cast<bool>(session), "engine runs the manifest");
    if (session) {
        check(session.value().status == "completed", "session completes");
        check(session.value().runs.size() == 1, "one run recorded");
        check(!session.value().manifest_digest.empty(), "manifest digest recorded");
        if (!session.value().runs.empty()) {
            const RunRecord &run = session.value().runs.front();
            check(run.oracle.passed, "oracle passes");
            check(run.status == "completed", "run completes");
            std::size_t measured = 0;
            for (const auto &sample : run.samples) {
                if (sample.observation.metric == "wall" && sample.observation.valid)
                    ++measured;
            }
            check(measured == 3, "warm-up excluded, three measured samples");
            check(!render_terminal(session.value()).empty(), "terminal report renders");
            check(!render_markdown(session.value()).empty(), "markdown report renders");
            check(render_html(session.value()).rfind("<!DOCTYPE html>", 0) == 0,
                  "html report is standalone");
            check(!render_csv(session.value()).empty(), "csv report renders");
        }
    }

    auto invalid = engine.run_file(invalid_manifest);
    check(!invalid, "invalid manifest fails before execution");

    check(run_cli({"validate", invalid_manifest}) == 2, "validate exits 2 for invalid manifest");
    check(run_cli({"validate"}) == 2, "validate without a manifest exits 2");
    check(run_cli({"recommend"}) == 5, "recommend reports the honest unavailable state");

    const std::string store = (root / "store").string();
    check(run_cli({"run", manifest, "--store", store, "--format", "csv"}) == 0,
          "run exits 0 and commits");
    auto history = HistoryStore::open(store);
    check(static_cast<bool>(history), "history store opens");
    if (history) {
        auto listed = history.value().list();
        check(listed && listed.value().size() == 1, "run committed one session");
        if (listed && !listed.value().empty()) {
            const std::string id = listed.value().front().id;
            check(run_cli({"report", "--session", id, "--store", store}) == 0,
                  "report renders a committed session");
            check(run_cli({"compare", id, id, "--store", store}) == 0,
                  "comparing a session with itself is not a regression");
        }
    }
    check(run_cli({"report", "--session", "missing", "--store", store}) == 5,
          "missing session exits 5");
    check(run_cli({"history", "--store", store}) == 0, "history exits 0");

    std::filesystem::remove_all(root);

    if (failures != 0) {
        std::cerr << failures << " engine/CLI check(s) failed\n";
        return 1;
    }
    std::cout << "engine/CLI tests passed\n";
    return 0;
}
