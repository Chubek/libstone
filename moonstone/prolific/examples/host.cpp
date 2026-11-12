#include "prolific/Cache.hpp"
#include "prolific/LuaProfiler.hpp"

#include <iostream>

int main(int argc, char **argv) {
    using namespace moonstone::prolific;
    Recorder recorder;
    {
        Scope compile(recorder, Domain::Compiler, "compile", {"sample.lang", "module:1", 1, 1});
        Scope pass(recorder, Domain::Compiler, "constant-fold");
        if (recorder.emit(Domain::Compiler, Kind::Counter, "folded.nodes", 8) != Code::Ok)
            return 1;
    }
    {
        Scope call(recorder, Domain::VM, "evaluate", {"sample.lang", "expr:42", 2, 5});
        if (recorder.emit(Domain::VM, Kind::Counter, "opcode.add", 1) != Code::Ok)
            return 1;
        if (recorder.emit(Domain::VM, Kind::Error, "division-by-zero", 0,
                          "host observed a zero divisor",
                          {"sample.lang", "expr:43", 2, 9}) != Code::Ok)
            return 1;
    }
    auto report = analyze(recorder.snapshot(), R"(
        local vm = require('lprofvm')
        vm.on('error', function(e) vm.finding('error', 'VM-FAULT', e.detail) end)
    )");
    if (argc == 2) {
        Cache cache(argv[1]);
        cache.save("example", report);
    }
    std::cout << report.json() << '\n';
    return report.analysis_status == "completed" ? 0 : 1;
}
