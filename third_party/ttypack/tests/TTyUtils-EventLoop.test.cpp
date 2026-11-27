#include <TTyUtils/TTyUtils-EventLoop.hpp>
#include <cassert>
#include <chrono>
#include <thread>

int main() {
    using namespace std::chrono_literals;
    namespace loop = ttyutils::loop;
    loop::Loop event_loop;
    {
        loop::Loop other;
        assert(loop::Loop::current() == &other);
    }
    assert(loop::Loop::current() == &event_loop);

    int calls = 0;
    auto cancelled = event_loop.after(0ms, [&] { assert(false); });
    auto copy = cancelled;
    copy.cancel();
    assert(!cancelled.active());
    auto first = event_loop.after(0ms, [&] {
        ++calls;
        (void)event_loop.after(0ms, [&] { ++calls; });
    });
    event_loop.run_once(0ms);
    assert(calls == 1 && !first.active());
    event_loop.run_once(0ms);
    assert(calls == 2);

    auto idle = event_loop.idle([&] { ++calls; (void)event_loop.idle([&] { ++calls; }); });
    event_loop.run_once(0ms);
    assert(calls == 3);
    idle.cancel();
    event_loop.run_once(0ms);
    assert(calls == 4);

    int pipe_fds[2]{};
    assert(::pipe(pipe_fds) == 0);
    auto watch = event_loop.watch(pipe_fds[0], 0, [&](const loop::Event& event) {
        char byte{};
        assert(event.kind == loop::Event::Kind::readable && event.ready & POLLIN);
        assert(::read(event.fd, &byte, 1) == 1 && byte == 'a');
        ++calls;
    });
    assert(::write(pipe_fds[1], "a", 1) == 1);
    event_loop.run_once(0ms);
    assert(calls == 5); // idle, not the disabled watch
    watch.set_events(loop::Readable);
    event_loop.run_once(0ms);
    assert(calls == 6);
    watch.cancel();
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);

    int posted = 0;
    std::thread producer([&] {
        for (int i = 0; i < 80000; ++i) loop::post(event_loop, [&] { ++posted; });
    });
    producer.join(); // Filling the self-pipe must not block a producer.
    event_loop.run_once(0ms);
    assert(posted == 80000);

    bool timed = false;
    auto timer = event_loop.after(3ms, [&] { timed = true; });
    event_loop.run_once(1s);
    assert(timed && !timer.active());
    return 0;
}
