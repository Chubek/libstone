#include "prolific/Prolific.hpp"

#include <cassert>
#include <limits>
#include <thread>

using namespace moonstone::prolific;
int main() {
    Limits small;
    small.text = 16;
    Recorder admission(small);
    const auto parent = admission.begin(Domain::VM, "parent");
    const std::string oversized(1024 * 1024, 'x');
    assert(admission.begin(Domain::VM, oversized).code == Code::Exhausted);
    assert(admission.emit(Domain::VM, Kind::Instant, "payload", 0, oversized) == Code::Exhausted);
    assert(admission.emit(Domain::VM, Kind::Instant, "source", 0, {}, {oversized, {}, 0, 0}) ==
           Code::Exhausted);
    assert(admission.end(parent.span) == Code::Ok);
    const auto recovered = admission.snapshot();
    assert(recovered.events.size() == 2 && recovered.open_spans == 0 && recovered.rejected == 3);
    Report contradictory{recovered, {{"note", "TEST", "finding", 1}}, "failed", "cause"};
    for (const auto *status : {"failed", "not_requested"}) {
        contradictory.analysis_status = status;
        contradictory.analysis_error = std::string(status) == "failed" ? "cause" : "";
        bool rejected = false;
        try {
            (void)encode(contradictory);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
        rejected = false;
        try {
            (void)contradictory.json();
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
    }
    for (const auto limits : {Limits{0, 1, 1, 1, 1}, Limits{1000001, 1, 1, 1, 1},
                              Limits{1, 1, 1024 * 1024 + 1, 1, 1}}) {
        bool rejected = false;
        try {
            Recorder invalid_limits(limits);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        assert(rejected);
    }
    Recorder recorder;
    auto outer = recorder.begin(Domain::Compiler, "compile", {"input\".lang", "ir:α", 9, 3});
    auto inner = recorder.begin(Domain::Compiler, "fold");
    assert(outer.code == Code::Ok && inner.code == Code::Ok);
    assert(recorder.emit(Domain::VM, Kind::Counter, "opcode.add", INT64_MIN) == Code::Ok);
    assert(recorder.emit(Domain::VM, Kind::Error, "fault", 0, "zero\n\t\"\\\1") == Code::Ok);
    assert(recorder.end(inner.span) == Code::Ok);
    assert(recorder.end(outer.span) == Code::Ok);
    auto snapshot = recorder.snapshot();
    assert(snapshot.complete() && snapshot.events.size() == 6);
    assert(snapshot.events[1].parent == outer.span);
    assert(snapshot.events[4].duration_ns >= snapshot.events[4].self_ns);
    assert(snapshot.events[5].self_ns ==
           snapshot.events[5].duration_ns - snapshot.events[4].duration_ns);
    assert(snapshot.events[5].location.ir_id == "ir:α");
    Report report;
    report.data = snapshot;
    const auto json = report.json();
    assert(json.find("\\u0001") != std::string::npos &&
           json.find("\\n\\t\\\"\\\\") != std::string::npos);
    auto encoded = encode(report);
    assert(decode(encoded).json() == json);
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        bool failed = false;
        try {
            (void)decode(std::string_view(encoded).substr(0, i));
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed);
    }
    auto corrupt = encoded;
    corrupt[8] = 2;
    for (auto data : {corrupt, encoded + "garbage"}) {
        bool failed = false;
        try {
            (void)decode(data);
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed);
    }
    bool failed = false;
    try {
        (void)decode(encoded, 1);
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed);
    report.data.events[1].parent = 999;
    failed = false;
    try {
        (void)encode(report);
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed);

    Recorder invalid;
    const auto a = invalid.begin(Domain::VM, "a");
    const auto b = invalid.begin(Domain::VM, "b");
    assert(invalid.end(a.span) == Code::InvalidInput);
    std::thread other([&] { assert(invalid.end(b.span) == Code::InvalidInput); });
    other.join();
    assert(invalid.end(b.span) == Code::Ok && invalid.end(a.span) == Code::Ok);
    assert(invalid.end(a.span) == Code::InvalidInput);
    assert(invalid.emit(Domain::VM, Kind::Begin, "bad") == Code::InvalidInput);
    assert(invalid.emit(static_cast<Domain>(99), Kind::Instant, "bad") == Code::InvalidInput);
    assert(invalid.emit(Domain::VM, Kind::Instant, "") == Code::InvalidInput);
    assert(invalid.emit(Domain::VM, Kind::Instant, "\xc0\x80") == Code::InvalidInput);
    assert(invalid.snapshot().rejected == 7 && !invalid.snapshot().complete());
    assert(decode(encode(Report{invalid.snapshot(), {}, "not_requested", {}})).data.rejected == 7);

    Limits limits;
    limits.events = 1;
    Recorder event_bound(limits);
    auto pending = event_bound.begin(Domain::VM, "open");
    assert(pending.code == Code::Ok && event_bound.end(pending.span) == Code::Exhausted);
    assert(event_bound.snapshot().open_spans == 1 && !event_bound.snapshot().complete());
    limits = {};
    limits.bytes = 1;
    Recorder byte_bound(limits);
    assert(byte_bound.emit(Domain::VM, Kind::Instant, "x") == Code::Exhausted);
    limits = {};
    limits.text = 1;
    Recorder text_bound(limits);
    assert(text_bound.emit(Domain::VM, Kind::Instant, "xx") == Code::Exhausted);
    limits = {};
    limits.depth = 1;
    Recorder depth_bound(limits);
    assert(depth_bound.begin(Domain::VM, "a").code == Code::Ok);
    assert(depth_bound.begin(Domain::VM, "b").code == Code::Exhausted);
    limits = {};
    limits.threads = 1;
    Recorder thread_bound(limits);
    std::thread first(
        [&] { assert(thread_bound.emit(Domain::VM, Kind::Instant, "a") == Code::Ok); });
    first.join();
    std::thread second(
        [&] { assert(thread_bound.emit(Domain::VM, Kind::Instant, "b") == Code::Exhausted); });
    second.join();
    assert(thread_bound.snapshot().rejected == 1);

    Recorder concurrent;
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i)
        workers.emplace_back([&] {
            for (int n = 0; n < 100; ++n) {
                Scope scope(concurrent, Domain::VM, "call");
                assert(scope.result().code == Code::Ok);
                assert(concurrent.emit(Domain::VM, Kind::Allocation, "object", 16) == Code::Ok);
                assert(concurrent.emit(Domain::VM, Kind::Free, "object", 16) == Code::Ok);
            }
        });
    for (int i = 0; i < 10; ++i)
        (void)encode(Report{concurrent.snapshot(), {}, "not_requested", {}});
    for (auto &worker : workers)
        worker.join();
    snapshot = concurrent.snapshot();
    assert(snapshot.complete() && snapshot.events.size() == 3200);
    assert(decode(encode(Report{snapshot, {}, "not_requested", {}})).data.events.size() == 3200);
    concurrent.record_loss();
    assert(!concurrent.snapshot().complete());
    Recorder overflow;
    for (auto value : {INT64_MAX, INT64_C(1), INT64_MIN})
        assert(overflow.emit(Domain::VM, Kind::Counter, "overflow", value) == Code::Ok);
    Report overflow_report{overflow.snapshot(), {}, "not_requested", {}};
    assert(overflow_report.json().find("\"sum\":null") != std::string::npos);
    assert(overflow_report.json().find("\"overflow\":true") != std::string::npos);
}
