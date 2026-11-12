#include "prolific/Speculation.hpp"
#include "prolific/Cache.hpp"
#include "prolific/LuaProfiler.hpp"

#include <cassert>
#include <functional>
#include <iostream>

using namespace moonstone::prolific;
namespace {
void fails(const std::function<void()> &fn) {
    bool failed = false;
    try {
        fn();
    } catch (const std::exception &) {
        failed = true;
    }
    assert(failed);
}
Snapshot fixture() {
    Recorder recorder;
    for (unsigned k = 0; k < 11; ++k) {
        const auto kind = static_cast<ProfileKind>(k);
        Observation o{
            kind, "site:" + std::string(name(kind)), "build-v1/x86/workload-A", "operand", "", "",
            0};
        if (numeric(kind))
            o.unit = kind == ProfileKind::Cost         ? "ns"
                     : kind == ProfileKind::Allocation ? "bytes"
                                                       : "i64";
        else
            o.outcome = kind == ProfileKind::Branch ? "true" : "fast";
        if (kind == ProfileKind::Custom)
            o.outcome = std::string("\"); _G.PWNED = true; --\n") + '\0' + "é";
        for (int i = 0; i < 40; ++i) {
            if (kind == ProfileKind::Value)
                o.value = INT64_MAX;
            if (kind == ProfileKind::Cost)
                o.value = 1000000 + i;
            if (kind == ProfileKind::Allocation)
                o.value = i % 2 == 0 ? 16 : 32;
            assert(observe(recorder, Domain::VM, o, {"file\".lang", "ir:α", 12, 3}) == Code::Ok);
        }
    }
    // Distinct contexts, dimensions, units and source identities must not pool.
    for (int i = 0; i < 32; ++i)
        assert(observe(recorder, Domain::VM,
                       {ProfileKind::Value, "signed-range", "ctx", "value", "", "i64",
                        i % 2 ? INT64_MIN : INT64_MAX}) == Code::Ok);
    for (const auto *context : {"build-2", "build-3"})
        for (int i = 0; i < 3; ++i)
            assert(observe(recorder, Domain::Compiler,
                           {ProfileKind::Type, "same", context, "receiver", "T", "", 0}) ==
                   Code::Ok);
    for (const auto *dimension : {"lhs", "rhs"})
        assert(observe(recorder, Domain::VM,
                       {ProfileKind::Type, "same", "ctx", dimension, "T", "", 0}) == Code::Ok);
    for (const auto *unit : {"ns", "cycles"})
        assert(observe(recorder, Domain::VM,
                       {ProfileKind::Cost, "same", "ctx", "metric", "", unit, 1}) == Code::Ok);
    for (const auto *ir : {"1", "2"})
        assert(observe(recorder, Domain::VM,
                       {ProfileKind::Type, "same", "ctx", "source", "T", "", 0},
                       {"", ir, 0, 0}) == Code::Ok);
    return recorder.snapshot();
}
} // namespace
int main(int argc, char **argv) {
    auto snapshot = fixture();
    auto lua = speculate(snapshot);
    assert(lua == speculate(snapshot));
    assert(lua.find("local ir = require('lspecir')") == 0);
    assert(lua.find("SUM-OVERFLOW") != std::string::npos);
    assert(lua.find("SEMANTICS-REQUIRED") != std::string::npos);
    for (const auto *category : {"type", "branch", "value", "call", "interoperation", "system",
                                 "cost", "lowering", "allocation", "exception", "custom"})
        assert(lua.find("category=\"" + std::string(category) + "\"") != std::string::npos);
    Report report{snapshot, {}, "not_requested", {}};
    assert(speculate(decode(encode(report)).data) == lua);
    assert(!profile_observation(Event{}));
    const auto observation = profile_observation(snapshot.events.front());
    assert(observation && observation->kind == ProfileKind::Type && observation->outcome == "fast");
    assert(analyze(snapshot, R"(
        local vm = require('lprofvm')
        local n = 0
        vm.on('instant', function(e)
            assert(e.profile and e.profile.site and e.profile.context)
            n = n + 1
        end)
        vm.finish(function() assert(n > 400) end)
    )")
               .analysis_status == "completed");

    auto incomplete = snapshot;
    incomplete.rejected = 1;
    auto partial = speculate(incomplete);
    assert(partial.find("INCOMPLETE") != std::string::npos &&
           partial.find("ir.guard") == std::string::npos &&
           partial.find("ir.advice") == std::string::npos);
    Recorder open;
    open.begin(Domain::VM, "open");
    assert(speculate(open.snapshot()).find("INCOMPLETE") != std::string::npos);
    assert(speculate(Snapshot{}).find("NO-PROFILES") != std::string::npos);

    Recorder cases;
    for (int i = 0; i < 40; ++i) {
        assert(observe(cases, Domain::VM,
                       {ProfileKind::Branch, "balanced", "ctx", "condition",
                        i % 2 ? "true" : "false", "", 0}) == Code::Ok);
        assert(observe(cases, Domain::VM,
                       {ProfileKind::Type, "dominant", "ctx", "receiver", i < 38 ? "T" : "U", "",
                        0}) == Code::Ok);
    }
    auto mixed = speculate(cases.snapshot());
    assert(mixed.find("POLYMORPHIC") != std::string::npos &&
           mixed.find("ir.guard") != std::string::npos);
    SpeculationPolicy policy;
    policy.dominance_permille = 951;
    assert(speculate(cases.snapshot(), policy).find("ir.guard") == std::string::npos);
    policy = {};
    policy.buckets_per_profile = 1;
    auto truncated = speculate(cases.snapshot(), policy);
    assert(truncated.find("TRUNCATED") != std::string::npos &&
           truncated.find("ir.guard") == std::string::npos);
    for (const auto *reserved :
         {"prolific.profile/1", "prolific.profile/2", "prolific.profile/999"}) {
        Recorder bad;
        bad.emit(Domain::VM, Kind::Instant, reserved, 0, "malformed");
        fails([&] { (void)speculate(bad.snapshot()); });
    }
    auto corrupt = snapshot;
    corrupt.events[0].detail += "trailing";
    fails([&] { (void)speculate(corrupt); });
    corrupt = snapshot;
    corrupt.events[0].value = 1;
    fails([&] { (void)speculate(corrupt); });
    Recorder bad;
    for (const Observation &o :
         {Observation{ProfileKind::Branch, "site", "ctx", "condition", "maybe", "", 0},
          Observation{ProfileKind::Type, "site", "", "operand", "T", "", 0},
          Observation{ProfileKind::Cost, "site", "ctx", "cost", "", "ns", -1},
          Observation{ProfileKind::Value, "site", "ctx", "value", "", "", 1},
          Observation{static_cast<ProfileKind>(99), "site", "ctx", "x", "T", "", 0}})
        assert(observe(bad, Domain::VM, o) == Code::InvalidInput);
    assert(bad.snapshot().rejected == 5 && bad.snapshot().events.empty());
    Observation huge{ProfileKind::Custom,           "site", "ctx", "dimension",
                     std::string(1024 * 1024, 'x'), "",     0};
    assert(observe(bad, Domain::VM, huge) == Code::Exhausted);
    assert(bad.snapshot().rejected == 6);
    policy = {};
    policy.profiles = 1;
    fails([&] { (void)speculate(snapshot, policy); });
    policy = {};
    policy.instructions = 1;
    fails([&] { (void)speculate(snapshot, policy); });
    policy = {};
    policy.output_bytes = 16;
    fails([&] { (void)speculate(snapshot, policy); });
    policy = {};
    policy.minimum_samples = 0;
    fails([&] { (void)speculate(snapshot, policy); });
    policy = {};
    policy.dominance_permille = 500;
    fails([&] { (void)speculate(snapshot, policy); });
    if (argc == 2) {
        Cache cache(argv[1]);
        cache.save("profiles", report);
        report.data = incomplete;
        cache.save("incomplete", report);
        report.data = cases.snapshot();
        cache.save("mixed", report);
    }
}
