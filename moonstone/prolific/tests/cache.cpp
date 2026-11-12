#include "prolific/Cache.hpp"
#include "prolific/prolific.h"

#include <cassert>
#include <chrono>
#include <thread>

using namespace moonstone::prolific;
template <class F> void fails(F &&f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }
    assert(failed);
}
int main() {
    const auto path = std::filesystem::current_path() /
                      ("prolific-cache-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Recorder recorder;
    assert(recorder.emit(Domain::VM, Kind::Counter, "dispatch", INT64_MAX) == Code::Ok);
    Report report{recorder.snapshot(), {}, "not_requested", {}};
    {
        Cache cache(path, false, 64 * 1024);
        cache.save("first", report);
        assert(cache.json("first") == report.json());
        assert(cache.load("first")->data.events[0].value == INT64_MAX);
        assert(!cache.load("missing") && !cache.json("missing"));
        assert(cache.runs() == std::vector<std::string>{"first"});
        fails([&] { cache.save("first", report); });
        fails([&] { Cache duplicate(path); });
        fails([&] { cache.save("../bad", report); });
        fails([&] { cache.load(""); });
        Recorder large;
        for (int i = 0; i < 300; ++i)
            assert(large.emit(Domain::VM, Kind::Instant, "large", 0, std::string(4096, 'x')) ==
                   Code::Ok);
        Report oversized{large.snapshot(), {}, "not_requested", {}};
        fails([&] { cache.save("first", oversized, true); });
        assert(cache.json("first") == report.json()); // failed replacement rolled back both keys
        fails([&] { cache.save("too-big", oversized); });
        assert(!cache.load("too-big"));
        cache.save("second", report);
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i)
            readers.emplace_back([&] {
                for (int n = 0; n < 20; ++n)
                    assert(cache.load("first")->json() == report.json());
            });
        for (auto &reader : readers)
            reader.join();
    }
    {
        Cache reopened(path, true);
        assert(reopened.load("first")->json() == report.json());
        assert(reopened.runs().size() == 2);
        fails([&] { reopened.save("third", report); });
    }
    {
        Cache writer(path);
        report.analysis_status = "completed";
        writer.save("first", report, true);
        assert(writer.load("first")->analysis_status == "completed");
    }
    prolific_session *session = nullptr;
    assert(prolific_create(nullptr, &session) == PROLIFIC_OK);
    assert(prolific_emit(session, PROLIFIC_VM, PROLIFIC_ERROR, "C fault", 0, "cause", nullptr) ==
           PROLIFIC_OK);
    assert(prolific_save(session, path.string().c_str(), "c-abi", 0) == PROLIFIC_OK);
    assert(prolific_save(session, path.string().c_str(), "c-abi", 0) == PROLIFIC_FAILURE);
    assert(std::string(prolific_last_error()).find("already exists") != std::string::npos);
    assert(prolific_save(session, path.string().c_str(), "c-abi", 1) == PROLIFIC_OK);
    prolific_destroy(session);
    {
        Cache reader(path, true);
        assert(reader.load("c-abi")->data.events.front().detail == "cause");
    }
    fails([&] { Cache missing(path / "missing", true); });
    assert(!std::filesystem::exists(path / "missing"));
    std::filesystem::remove_all(path); // only the unique directory owned by this test
}
