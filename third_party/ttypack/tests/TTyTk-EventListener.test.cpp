#include <TTyTk/TTyTk-EventListener.hpp>

#include <cassert>

int main() {
    ttytk::EventBus bus;
    bool notified = false;
    bus.subscribe([&](const ttytk::Event&) { notified = true; });
    bus.post(ttytk::Resize{24, 80});
    ttytk::Event event;
    assert(bus.poll(event));
    assert(notified);
    const auto* resize = std::get_if<ttytk::Resize>(&event);
    assert(resize != nullptr && resize->columns == 80);
    assert(!bus.poll(event));

    ttytk::EventBus reentrant;
    unsigned first{}, inserted{};
    reentrant.subscribe({});
    reentrant.subscribe([&](const ttytk::Event& posted) {
        ++first;
        ttytk::Event queued;
        assert(reentrant.poll(queued));
        assert(queued.index() == posted.index());
        if (first == 1) {
            reentrant.subscribe([&](const ttytk::Event&) { ++inserted; });
            reentrant.post(ttytk::UserEvent{"next"});
        }
    });
    reentrant.post(ttytk::input::Paste{"paste"});
    assert(first == 2 && inserted == 1 && !reentrant.poll(event));
}
