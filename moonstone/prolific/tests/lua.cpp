#include "prolific/LuaProfiler.hpp"

#include <cassert>
#include <cstdlib>

#ifdef __linux__
// Deliberately conflicting host symbol. Any accidental binding to this instead
// of Prolific's private vendored Lua is an embedding ABI violation.
struct lua_State;
extern "C" lua_State *luaL_newstate(void) { std::abort(); }
#endif

using namespace moonstone::prolific;
int main() {
    Recorder recorder;
    {
        Scope pass(recorder, Domain::Compiler, "fold", {"source.lang", "ir:17", 7, 2});
        assert(recorder.emit(Domain::VM, Kind::Counter, "dispatch", 42) == Code::Ok);
        assert(recorder.emit(Domain::VM, Kind::Error, "fault", 0, "bad operand") == Code::Ok);
        assert(recorder.emit(Domain::Runtime, Kind::Error, "native") == Code::Ok);
    }
    const auto snapshot = recorder.snapshot();
    const auto script = R"(
        local cc, vm = require('lprofcc'), require('lprofvm')
        assert(cc.version == 1 and vm.version == 1)
        assert(os == nil and io == nil and debug == nil and package == nil and load == nil)
        cc.on('end', function(e)
            assert(e.name == 'fold' and e.location.ir_id == 'ir:17' and e.location.line == 7)
            assert(type(e.sequence) == 'string' and type(e.duration_ns) == 'string')
            cc.finding('note', 'HOT', 'inspect ' .. e.name)
        end)
        vm.on('counter', function(e)
            assert(e.value == 42)
            vm.finding('warning', 'COUNT', 'dispatch count')
        end)
        vm.on('error', function(e) vm.finding('error', 'FAULT', e.detail) end)
    )";
    auto report = analyze(snapshot, script);
    assert(report.analysis_status == "completed" && report.findings.size() == 3);
    assert(report.findings[0].code == "COUNT" && report.findings[0].event_sequence == 2);
    assert(report.findings[1].message == "bad operand" && report.findings[2].code == "HOT");
    assert(decode(encode(report)).json() == report.json());
    assert(analyze(snapshot, script).json() == report.json()); // no leaked VM state
    report = analyze(snapshot, R"(
        local vm = require('lprofvm')
        local n = 0
        vm.on('*', function() n = n + 1 end)
        vm.finish(function(summary)
            assert(n == 2 and summary.complete and summary.event_count == '5')
            vm.finding('note', 'SUMMARY', tostring(n))
        end)
    )");
    assert(report.analysis_status == "completed" && report.findings.size() == 1);
    assert(report.findings[0].event_sequence == 0 && report.findings[0].message == "2");
    for (const auto *bad : {"local =", "error('bad script')", "error('', 0)", "error({})",
                            "require('os')", "require('lprofvm').on('bogus', function() end)",
                            "require('lprofvm').on('error', function() error('bad callback') end)",
                            "require('lprofvm').finish(function() error('bad finish') end)",
                            "require('lprofvm').finding('error', 'EARLY', 'no event')",
                            "require('lprofvm').on('error', function() "
                            "require('lprofcc').finding('error','BAD','wrong domain') end)",
                            "require('lprofvm').on('error', function() require('lprofvm').on('*', "
                            "function() end) end)",
                            "require('lprofvm').on('error', function() "
                            "require('lprofvm').finding('bogus','BAD','severity') end)"}) {
        const auto failed = analyze(snapshot, bad);
        assert(failed.analysis_status == "failed" && !failed.analysis_error.empty() &&
               failed.findings.empty());
    }
    ScriptLimits limits;
    limits.instructions = 1000;
    assert(analyze(snapshot, "while true do end", limits).analysis_status == "failed");
    assert(analyze(snapshot, "require('lprofvm').on('*', function() while true do end end)", limits)
               .analysis_status == "failed");
    limits = {};
    limits.findings = 1;
    assert(analyze(snapshot, script, limits).analysis_status == "failed");
    assert(analyze(snapshot, R"(
        local vm = require('lprofvm')
        vm.on('error', function()
            for i = 1, 2 do pcall(vm.finding, 'note', 'LIMIT', 'caught overflow') end
        end)
    )",
                   limits)
               .analysis_status == "failed");
    assert(analyze(snapshot, R"(
        local vm = require('lprofvm')
        for i = 1, 257 do pcall(vm.on, '*', function() end) end
    )")
               .analysis_status == "failed");
    limits = {};
    limits.source_bytes = 1;
    assert(analyze(snapshot, script, limits).analysis_status == "failed");
    limits = {};
    limits.text = 1;
    assert(analyze(snapshot, script, limits).analysis_status == "failed");
    auto incomplete = snapshot;
    incomplete.rejected = 1;
    assert(!analyze(incomplete, script).data.complete());
    // Lua finalizers run while native callback captures are still alive.
    const auto finalized = analyze(snapshot, R"(
        local vm = require('lprofvm')
        finalizer = setmetatable({}, {__gc=function()
            pcall(function() require('lprofvm').finding('error', 'GC', 'not in an event') end)
        end})
    )");
    assert(finalized.analysis_status == "completed" && finalized.findings.empty());
}
