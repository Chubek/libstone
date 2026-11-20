from __future__ import annotations

from domainlet.vmm.io.event_loop import EventLoop
from domainlet.vmm.tests.fakes import FakeSubscriber


def test_event_loop_delivers_events() -> None:
    seen: list[int] = []
    sub = FakeSubscriber(callback=seen.append)
    loop = EventLoop()
    try:
        loop.register(sub)
        sub.trigger()
        assert loop.run_once(0.1) == 1
        assert seen
    finally:
        sub.close()
        loop.close()
