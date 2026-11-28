// Pins the additive v1.1 contract: versions, table shapes, id rules, and
// fingerprint determinism. No loader, runner, profile or recommender object is
// constructed here: those implementations land with their own workstream.
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

#include "mahak/Plugin.hpp"
#include "mahak/Profile.hpp"
#include "mahak/Recommend.hpp"
#include "mahak/Runner.hpp"

using namespace moonstone::mahak;

namespace {

int failures = 0;

void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
}

static_assert(mahak_plugin_abi_version == 1, "plugin ABI version is pinned");
static_assert(mahak_runner_api_version == 1, "runner API version is pinned");
static_assert(mahak_profile_schema_version == 1, "profile schema version is pinned");
static_assert(mahak_recommend_api_version == 1, "recommend API version is pinned");

static_assert(std::is_trivially_copyable_v<StringView>, "StringView crosses the ABI by value");
static_assert(std::is_trivially_copyable_v<PluginDescriptor>,
              "PluginDescriptor crosses the ABI by value");
static_assert(std::is_trivially_copyable_v<PluginHooks>,
              "PluginHooks crosses the ABI by value");
static_assert(std::is_trivially_copyable_v<PluginHostApi>,
              "PluginHostApi crosses the ABI by value");

static_assert(PluginDescriptor{}.abi_version == mahak_plugin_abi_version,
              "descriptor defaults to the current ABI");
static_assert(PluginHostApi{}.abi_version == mahak_plugin_abi_version,
              "host table defaults to the current ABI");

} // namespace

int main() {
    const std::string_view entry = mahak_plugin_entry_symbol;
    check(entry == "mahak_plugin_init_v1", "the entry symbol is stable");

    // Size-tagged tables are the ABI compatibility mechanism; each default must
    // describe the table it lives in.
    const PluginDescriptor descriptor;
    const PluginHooks hooks;
    const PluginHostApi host;
    const PluginRegistration registration;
    check(descriptor.struct_size == sizeof(PluginDescriptor),
          "descriptor is size-tagged by default");
    check(hooks.struct_size == sizeof(PluginHooks), "hooks table is size-tagged by default");
    check(host.struct_size == sizeof(PluginHostApi), "host table is size-tagged by default");
    check(registration.struct_size == sizeof(PluginRegistration),
          "registration is size-tagged by default");
    check(host.abi_version == mahak_plugin_abi_version, "host table declares the ABI");

    check(make_string_view("abc").value() == "abc", "StringView round-trips bytes");
    check(StringView{}.value().empty(), "a null StringView is empty");
    check(has_capability(PluginCapability::Judge | PluginCapability::Deterministic,
                         PluginCapability::Deterministic),
          "capability flags compose");
    check(!has_capability(PluginCapability::Judge, PluginCapability::Report),
          "unrelated capabilities are not reported");

    check(valid_plugin_id("acme.oracle.checksum"), "qualified plugin ids are accepted");
    check(valid_plugin_id("cephyr.abi.aggregate"), "three-part ids are accepted");
    check(!valid_plugin_id("Acme.oracle"), "upper-case ids are rejected");
    check(!valid_plugin_id("acme"), "an unqualified id is rejected");
    check(!valid_plugin_id(".acme.x"), "a leading dot is rejected");
    check(!valid_plugin_id("acme..x"), "an empty component is rejected");
    check(!valid_plugin_id("acme/x"), "path characters are rejected");

    for (PluginKind kind : {PluginKind::FixtureGenerator, PluginKind::Oracle,
                            PluginKind::Measurement, PluginKind::Analysis,
                            PluginKind::Predictor, PluginKind::Reporter}) {
        const std::string text = name(kind);
        const auto parsed = parse_plugin_kind(text);
        check(parsed && *parsed == kind, "plugin kind name round-trips: " + text);
    }
    check(!parse_plugin_kind("nonsense"), "unknown plugin kinds are rejected");

    for (HookKind kind : {HookKind::Configure, HookKind::Generate, HookKind::Judge,
                          HookKind::Measure, HookKind::Analyze, HookKind::Predict,
                          HookKind::Report}) {
        const std::string text = name(kind);
        const auto parsed = parse_hook_kind(text);
        check(parsed && *parsed == kind, "hook name round-trips: " + text);
    }
    for (HookComposition composition : {HookComposition::LastWins,
                                        HookComposition::AllMustPass,
                                        HookComposition::FirstPass}) {
        const std::string text = name(composition);
        const auto parsed = parse_hook_composition(text);
        check(parsed && *parsed == composition, "composition round-trips: " + text);
    }
    check(!parse_hook_kind("configure-later"), "unknown hooks are rejected");

    for (FactKind kind : {FactKind::Language, FactKind::Semantics, FactKind::Prerequisite,
                          FactKind::Target, FactKind::Pass, FactKind::Exclusion,
                          FactKind::Evidence}) {
        const std::string text = name(kind);
        const auto parsed = parse_fact_kind(text);
        check(parsed && *parsed == kind, "fact kind round-trips: " + text);
    }
    check(std::string(name(RecommendationStatus::BudgetExhausted)) ==
              "budget-exhausted",
          "a solver budget stop is never reported as success");
    check(std::string(name(SolverOutcome::Unsatisfiable)) == "unsatisfiable",
          "unsatisfiable is a distinct outcome");

    auto profile_id = make_profile_id("Cephyr Optimization!");
    check(static_cast<bool>(profile_id), "a display name yields a profile id");
    if (profile_id) {
        check(profile_id.value().stable == "cephyr-optimization",
              "the derived id is lower-case and separator-collapsed");
        check(profile_id.value().display == "Cephyr Optimization!",
              "the display name is preserved verbatim");
        check(valid_profile_id(profile_id.value().stable), "the derived id is valid");
    }
    check(!make_profile_id("!!!"), "a display name with no usable characters is rejected");
    check(!valid_profile_id("../escape"), "unsafe profile ids are rejected");
    auto digit_first = make_profile_id("2026 run");
    check(digit_first && digit_first.value().stable.front() != '-',
          "a digit-leading id is still a valid directory name");

    FingerprintInput first;
    first.executable_digest = "abc123";
    first.argv = {"prog", "64"};
    first.fixture_digest = "fixture";
    first.seed = 7;
    first.target_abi = "x86_64-linux-gnu";
    auto fingerprint_a = fingerprint_execution(first);
    auto fingerprint_b = fingerprint_execution(first);
    check(fingerprint_a && fingerprint_b, "fingerprints compute");
    if (fingerprint_a && fingerprint_b) {
        check(fingerprint_a.value().valid, "a computed fingerprint is valid");
        check(fingerprint_a.value().digest == fingerprint_b.value().digest,
              "fingerprints are deterministic");
        check(fingerprint_a.value().digest.size() == 16, "digest is 16 hex characters");
        FingerprintInput changed = first;
        changed.seed = 8;
        auto fingerprint_c = fingerprint_execution(changed);
        check(fingerprint_c && fingerprint_c.value().digest != fingerprint_a.value().digest,
              "the seed participates in the fingerprint");
        FingerprintInput reordered = first;
        reordered.argv = {"prog", "128"};
        auto fingerprint_d = fingerprint_execution(reordered);
        check(fingerprint_d && fingerprint_d.value().digest != fingerprint_a.value().digest,
              "argv participates in the fingerprint");
        check(fingerprint_a.value().components.find("target_abi") != nullptr,
              "components are inspectable, not opaque");
    }

    OracleFacts facts;
    check(!validate_facts(facts), "facts without a language or semantics are rejected");
    facts.language = "C11";
    facts.semantics = "defined bitvectors, LP64";
    check(static_cast<bool>(validate_facts(facts)), "minimal facts validate");
    facts.facts.push_back(OracleFact{"cephyr.phi.fold", FactKind::Pass, "phi folding exists",
                                     "DESIGN.md", true});
    check(static_cast<bool>(validate_facts(facts)), "a fact with provenance validates");
    facts.facts.push_back(facts.facts.front());
    check(!validate_facts(facts), "duplicate fact ids are rejected");

    if (failures != 0) {
        std::cerr << failures << " contract check(s) failed\n";
        return 1;
    }
    std::cout << "contract tests passed\n";
    return 0;
}
