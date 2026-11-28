// Cephyr suite manifests must stay valid and plannable without running tools.
#include <iostream>
#include <string>

#include "mahak/Manifest.hpp"

#ifndef MAHAK_SUITE_DIR
#define MAHAK_SUITE_DIR "suites"
#endif

using moonstone::Diagnostics;
using moonstone::Severity;
using namespace moonstone::mahak;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

std::string suite(std::string_view name) {
    return std::string(MAHAK_SUITE_DIR) + "/" + std::string(name);
}

Result<SuitePlan> plan_of(const std::string &path, PlanOptions options = {}) {
    auto manifest = load_manifest(path);
    if (!manifest)
        return manifest.error();
    const Diagnostics diagnostics = validate_manifest(manifest.value());
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Error)
            return Error{ErrorCategory::InvalidInput, diagnostic.message, {}};
    }
    options.work_root = "/tmp/mahak-suite-plan";
    return plan_manifest(manifest.value(), options);
}

} // namespace

int main() {
    const std::string optimization = suite("cephyr-optimization.json");
    const std::string incremental = suite("cephyr-incremental.json");
    const std::string abi = suite("cephyr-abi.json");

    auto optimization_plan = plan_of(optimization);
    check(static_cast<bool>(optimization_plan), "optimization suite plans");
    if (optimization_plan) {
        check(optimization_plan.value().cases.size() == 14,
              "optimization suite covers 2 targets x 7 benchmarks");
        bool saw_baseline = false;
        bool saw_candidate = false;
        for (const auto &case_plan : optimization_plan.value().cases) {
            saw_baseline = saw_baseline || case_plan.target_id == "cephyr-baseline";
            saw_candidate = saw_candidate || case_plan.target_id == "cephyr-candidate";
            check(case_plan.stages.size() == 2, "optimization case builds and runs");
            check(!case_plan.fixtures.empty(), "optimization case carries a fixture");
        }
        check(saw_baseline && saw_candidate, "both compiler configurations are present");
    }

    auto incremental_plan = plan_of(incremental);
    check(static_cast<bool>(incremental_plan), "incremental suite plans");
    if (incremental_plan) {
        check(incremental_plan.value().cases.size() == 2, "incremental suite has two cases");
        for (const auto &case_plan : incremental_plan.value().cases) {
            if (case_plan.benchmark_id == "single-unit") {
                check(case_plan.stages.size() == 1 &&
                          case_plan.stages.front().kind == StageKind::Compile,
                      "single-unit is compile-only");
            } else {
                check(case_plan.stages.size() == 3, "full-build compiles, links and runs");
            }
        }
    }

    PlanOptions pinned;
    pinned.parameter_overrides = {{"compiler", "/opt/cephyr/bin/cephyr"}};
    auto pinned_plan = plan_of(optimization, pinned);
    check(static_cast<bool>(pinned_plan), "a suite-wide compiler override is accepted");
    if (pinned_plan) {
        check(pinned_plan.value().cases.size() == 14, "an override does not change the matrix");
        const auto &first = pinned_plan.value().cases.front();
        bool saw_override = false;
        for (const auto &parameter : first.parameters) {
            if (parameter.name == "compiler")
                saw_override = parameter.text == "/opt/cephyr/bin/cephyr";
        }
        check(saw_override, "the override value reaches the planned argv");
        check(first.stages.front().commands.front().argv.front() == "/opt/cephyr/bin/cephyr",
              "the override replaces the compiler argument");
    }
    auto abi_pinned = plan_of(abi, pinned);
    check(static_cast<bool>(abi_pinned), "an override declared by a benchmark is suite-wide");

    PlanOptions unknown;
    unknown.parameter_overrides = {{"nonesuch", "1"}};
    auto rejected = plan_of(optimization, unknown);
    check(!rejected, "unknown override names are rejected");

    if (failures != 0) {
        std::cerr << failures << " suite check(s) failed\n";
        return 1;
    }
    std::cout << "suite tests passed\n";
    return 0;
}
