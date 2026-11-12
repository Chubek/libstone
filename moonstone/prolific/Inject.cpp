#include "Cache.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unistd.h>

namespace {
using namespace moonstone::prolific;
std::atomic<bool> stopped{false};
std::mutex lifecycle;
thread_local bool in_profiler = false;
struct Entry {
    void *function = nullptr;
    std::uint64_t span = 0;
};
struct Stack {
    std::array<Entry, 512> entries{};
    std::size_t depth = 0, skipped = 0;
};
thread_local Stack stack;
struct Injected {
    std::unique_ptr<Recorder> recorder;
    std::string directory, run;
    Injected() {
        const auto *path = std::getenv("PROLIFIC_CACHE");
        if (!path || !*path)
            return; // explicitly disabled, not an empty successful trace
        directory = path;
        recorder = std::make_unique<Recorder>();
        if (const auto *id = std::getenv("PROLIFIC_RUN"))
            run = id;
        else
            run = "native-" + std::to_string(getpid()) + "-" +
                  std::to_string(recorder->snapshot().created_unix_ns);
    }
    ~Injected() {
        std::lock_guard lock(lifecycle);
        stopped.store(true, std::memory_order_release);
        if (!recorder)
            return;
        try {
            Report report;
            report.data = recorder->snapshot();
            Cache cache(directory);
            cache.save(run, report);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "Prolific injection: cannot persist trace: %s\n", error.what());
        } catch (...) {
            std::fputs("Prolific injection: cannot persist trace (unknown failure)\n", stderr);
        }
    }
};
Injected &injected() {
    static Injected instance;
    return instance;
}
struct Guard {
    Guard() { in_profiler = true; }
    ~Guard() { in_profiler = false; }
};
void failed(Recorder *recorder) noexcept {
    try {
        std::lock_guard lock(lifecycle);
        if (!stopped.load(std::memory_order_acquire) && recorder)
            recorder->record_loss();
        stopped.store(true, std::memory_order_release);
    } catch (...) {
        stopped.store(true, std::memory_order_release);
    }
    std::fputs("Prolific injection: recording failed; profiling disabled\n", stderr);
}
} // namespace

// This translation unit (and all profiler dependencies) must not themselves
// be instrumented. The CMake target explicitly disables function instrumentation.
extern "C" void __cyg_profile_func_enter(void *function, void *) noexcept {
    if (in_profiler || stopped.load(std::memory_order_acquire))
        return;
    Guard guard;
    Recorder *recorder = nullptr;
    try {
        std::lock_guard lock(lifecycle);
        if (stopped.load(std::memory_order_acquire))
            return;
        auto &state = injected();
        if (!state.recorder)
            return;
        recorder = state.recorder.get();
        if (stack.depth == stack.entries.size() || stack.skipped) {
            ++stack.skipped;
            state.recorder->record_loss();
            return;
        }
        char identity[2 + sizeof(void *) * 2 + 1];
        std::snprintf(identity, sizeof(identity), "%p", function);
        auto result = state.recorder->begin(Domain::Runtime, identity, {"", identity, 0, 0});
        stack.entries[stack.depth++] = {function, result.span};
    } catch (...) {
        failed(recorder);
    }
}
extern "C" void __cyg_profile_func_exit(void *function, void *) noexcept {
    if (in_profiler || stopped.load(std::memory_order_acquire))
        return;
    Guard guard;
    Recorder *recorder = nullptr;
    try {
        std::lock_guard lock(lifecycle);
        if (stopped.load(std::memory_order_acquire))
            return;
        auto &state = injected();
        if (!state.recorder)
            return;
        recorder = state.recorder.get();
        if (stack.skipped) {
            --stack.skipped;
            return;
        }
        if (!stack.depth || stack.entries[stack.depth - 1].function != function) {
            state.recorder->record_loss();
            return;
        }
        const auto entry = stack.entries[--stack.depth];
        if (entry.span)
            state.recorder->end(entry.span);
    } catch (...) {
        failed(recorder);
    }
}
