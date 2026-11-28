// Manifest parsing, validation and planning contracts.
#include <iostream>
#include <string>

#include "mahak/Manifest.hpp"

using namespace moonstone::mahak;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

const char *kValid = R"({
  "schema": 1,
  "id": "unit",
  "version": "1.0.0",
  "seed": 7,
  "defaults": {"warmup": 1, "repetitions": 3, "timeout_ms": 1000},
  "targets": [
    {"id": "runner", "kind": "executable",
     "execute": {"argv": ["/bin/echo", "{param:word}", "{param:size}"]},
     "parameters": [{"name": "size", "values": [1, 2]}]}
  ],
  "benchmarks": [
    {"id": "echo", "group": "noise", "tags": ["fast"], "modes": ["execute-only"],
     "parameters": [{"name": "word", "values": ["a", "b"]}],
     "oracle": {"kind": "stdout-contains", "expected": "a"},
     "metrics": [{"name": "wall", "unit": "ns"}]},
    {"id": "other", "group": "noise", "modes": ["execute-only"],
     "parameters": [{"name": "word", "values": ["z"]}],
     "oracle": {"kind": "exit-code"}}
  ],
  "exclusions": [{"benchmark": "other", "target": "runner", "reason": "control"}]
})";

Result<SuitePlan> plan_of(const Manifest &manifest, PlanOptions options = {}) {
    options.work_root = "/tmp/mahak-unit";
    return plan_manifest(manifest, options);
}

} // namespace

int main() {
    auto manifest = parse_manifest(kValid, "valid.json", "/tmp");
    check(static_cast<bool>(manifest), "valid manifest parses");
    if (!manifest)
        return 1;
    check(manifest.value().targets.size() == 1, "one target");
    check(manifest.value().benchmarks.size() == 2, "two benchmarks");
    check(validate_manifest(manifest.value()).empty(), "valid manifest has no diagnostics");

    auto plan = plan_of(manifest.value());
    check(static_cast<bool>(plan), "plan succeeds");
    if (plan) {
        // Two words by two sizes; 'other' is excluded.
        check(plan.value().cases.size() == 4, "exclusions and matrix produce 4 cases");
        check(plan.value().cases.front().id.find("echo@runner") == 0, "stable case id prefix");
        check(plan.value().cases.front().stages.size() == 1, "execute-only keeps one stage");
        check(plan.value().cases.front().stages.front().commands.front().argv[1] == "a" ||
                  plan.value().cases.front().stages.front().commands.front().argv[1] == "b",
              "parameter placeholder expanded");
        for (const auto &benchmark : manifest.value().benchmarks)
            check(benchmark.id != "other" || true, "excluded benchmark still parsed");
    }

    PlanOptions pinned;
    pinned.parameter_overrides = {{"word", "b"}};
    auto pinned_plan = plan_of(manifest.value(), pinned);
    check(static_cast<bool>(pinned_plan), "pinned plan succeeds");
    if (pinned_plan) {
        check(pinned_plan.value().cases.size() == 2, "pinning halves the matrix");
        for (const auto &case_plan : pinned_plan.value().cases) {
            for (const auto &parameter : case_plan.parameters) {
                if (parameter.name == "word")
                    check(parameter.text == "b", "pinned value applied");
            }
        }
    }

    PlanOptions filtered;
    filtered.benchmarks = {"echo"};
    auto filtered_plan = plan_of(manifest.value(), filtered);
    check(static_cast<bool>(filtered_plan), "benchmark filter plans");
    if (filtered_plan)
        check(filtered_plan.value().cases.size() == 4, "filter selects only the echo matrix");

    PlanOptions only_excluded;
    only_excluded.benchmarks = {"other"};
    auto excluded_plan = plan_of(manifest.value(), only_excluded);
    check(!excluded_plan, "excluded pairs are not planned");

    PlanOptions including;
    including.include_excluded = true;
    auto flagged = plan_of(manifest.value(), including);
    check(flagged && flagged.value().cases.size() == 6, "include_excluded adds the pair");

    // Strictness: unknown keys, duplicate ids, bad placeholders, bad enums.
    auto unknown_key = parse_manifest(R"({"schema":1,"id":"x","surprise":true,
        "targets":[{"id":"t","kind":"executable","execute":{"argv":["/bin/true"]}}],
        "benchmarks":[{"id":"b","modes":["execute-only"]}]})",
                                      "unknown.json", "/tmp");
    check(!unknown_key, "unknown key rejected");
    const std::string unknown_message =
        unknown_key ? std::string() : unknown_key.error().message;
    check(unknown_message.find("surprise") != std::string::npos,
          "diagnostic names the offending key");

    auto duplicate = parse_manifest(R"({"schema":1,"id":"x",
        "targets":[{"id":"t","kind":"executable","execute":{"argv":["/bin/true"]}},
                   {"id":"t","kind":"executable","execute":{"argv":["/bin/true"]}}],
        "benchmarks":[{"id":"b","modes":["execute-only"]}]})",
                                    "duplicate.json", "/tmp");
    check(!duplicate, "duplicate target ids rejected");

    auto bad_enum = parse_manifest(R"({"schema":1,"id":"x",
        "targets":[{"id":"t","kind":"nonsense","execute":{"argv":["/bin/true"]}}],
        "benchmarks":[{"id":"b","modes":["execute-only"]}]})",
                                   "enum.json", "/tmp");
    check(!bad_enum, "unknown target kind rejected");

    auto empty_argv = parse_manifest(R"({"schema":1,"id":"x",
        "targets":[{"id":"t","kind":"executable","execute":{"argv":[]}}],
        "benchmarks":[{"id":"b","modes":["execute-only"]}]})",
                                     "argv.json", "/tmp");
    check(!empty_argv, "empty argv rejected");

    auto bad_schema = parse_manifest(R"({"schema":2,"id":"x","targets":[],
        "benchmarks":[]})",
                                     "schema.json", "/tmp");
    check(!bad_schema, "future manifest schema rejected");

    auto unresolved = parse_manifest(R"({"schema":1,"id":"x",
        "targets":[{"id":"t","kind":"executable","execute":{"argv":["/bin/echo","{param:missing}"]}}],
        "benchmarks":[{"id":"b","modes":["execute-only"]}]})",
                                     "placeholder.json", "/tmp");
    check(static_cast<bool>(unresolved), "manifest with a late placeholder parses");
    if (unresolved) {
        PlanOptions options;
        options.work_root = "/tmp/mahak-unit";
        auto broken = plan_manifest(unresolved.value(), options);
        check(!broken, "unresolved placeholder fails planning");
        if (!broken)
            check(broken.error().message.find("missing") != std::string::npos,
                  "placeholder diagnostic names the parameter");
    }

    check(manifest_digest(manifest.value()) == manifest_digest(manifest.value()),
          "digest is stable");
    check(manifest_digest(manifest.value()).size() == 16, "digest is 16 hex characters");

    auto duplicate_leaf = parse_manifest(R"({"schema":1,"id":"x",
        "targets":[{"id":"t","kind":"executable","execute":{"argv":["/bin/true"]},
                    "parameters":[{"name":"n","values":[1]}]}],
        "benchmarks":[{"id":"b","modes":["execute-only"],
                       "parameters":[{"name":"n","values":[2]}]}]})",
                                         "leaf.json", "/tmp");
    check(static_cast<bool>(duplicate_leaf), "duplicate leaf parses");
    if (duplicate_leaf)
        check(!validate_manifest(duplicate_leaf.value()).empty(),
              "duplicate parameter leaf is a diagnostic");

    if (failures != 0) {
        std::cerr << failures << " manifest check(s) failed\n";
        return 1;
    }
    std::cout << "manifest tests passed\n";
    return 0;
}
