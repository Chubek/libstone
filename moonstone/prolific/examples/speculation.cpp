#include "prolific/Speculation.hpp"
#include "prolific/Cache.hpp"

#include <iostream>
#include <stdexcept>

// Illustrative host adapter data, not claims about a real optimizer/workload.
int main(int argc, char **argv) {
    using namespace moonstone::prolific;
    try {
        Recorder recorder;
        const std::string context = "example-build-1/x86-64/workload-A";
        for (int i = 0; i < 100; ++i) {
            const Observation samples[] = {
                {ProfileKind::Type, "add:receiver", context, "receiver-type",
                 i < 98 ? "Int64" : "Float64", "", 0},
                {ProfileKind::Branch, "loop:condition", context, "taken", i < 96 ? "true" : "false",
                 "", 0},
                {ProfileKind::Value, "shift:amount", context, "rhs", "", "i64", i < 97 ? 2 : 3},
                {ProfileKind::Call, "call:1", context, "target", "module::fast_add@v1", "", 0},
                {ProfileKind::Interoperation, "ffi:1", context, "boundary-contract",
                 "sysv64/add-i64/pure/owned@v1", "", 0},
                {ProfileKind::System, "read:1", context, "return-class",
                 i < 99 ? "success" : "EINTR", "", 0},
                {ProfileKind::Cost, "pass:fold", context, "elapsed", "", "ns", 1000000 + i * 20},
                {ProfileKind::Lowering, "lower:add", context, "selected-rule", "x86-64/addq@v1", "",
                 0},
                {ProfileKind::Allocation, "alloc:pair", context, "size", "", "bytes", 16},
                {ProfileKind::Exception, "decode:1", context, "exception-class", "InvalidEncoding",
                 "", 0},
                {ProfileKind::Custom, "cache:1", context, "lookup-outcome", "hit", "", 0},
            };
            for (const auto &sample : samples) {
                const auto domain =
                    sample.kind == ProfileKind::Lowering || sample.kind == ProfileKind::Cost
                        ? Domain::Compiler
                        : Domain::VM;
                if (observe(recorder, domain, sample, {"", sample.site, 0, 0}) != Code::Ok)
                    throw std::runtime_error("example profile admission failed");
            }
        }
        Report report{recorder.snapshot(), {}, "not_requested", {}};
        if (argc == 2) {
            Cache cache(argv[1]);
            cache.save("speculation-example", report);
        } else if (argc != 1)
            throw std::invalid_argument("usage: prolific-speculation [CACHE]");
        std::cout << speculate(report.data);
        return std::cout ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
