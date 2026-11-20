"""Thin epoll reactor."""

from __future__ import annotations

import select
from dataclasses import dataclass, field

from domainlet.vmm.errors import EventLoopError
from domainlet.vmm.seams import EventSubscriber


@dataclass(slots=True)
class EventLoop:
    """Level-triggered epoll wrapper."""

    _epoll: select.epoll = field(
        default_factory=select.epoll, init=False, repr=False)
    _subscribers: dict[int, EventSubscriber] = field(
        default_factory=dict, init=False, repr=False)

    def register(self, subscriber: EventSubscriber, events: int = select.EPOLLIN) -> None:
        """Register an event source."""

        fd = subscriber.fileno()
        if fd < 0 or events <= 0:
            raise EventLoopError("invalid event subscriber registration")
        if fd in self._subscribers:
            raise EventLoopError(f"subscriber fd {fd} is already registered")
        try:
            self._epoll.register(fd, events)
        except OSError as exc:
            raise EventLoopError(f"unable to register fd {fd}: {exc}") from exc
        self._subscribers[fd] = subscriber

    def unregister(self, subscriber: EventSubscriber) -> None:
        """Remove an event source."""

        fd = subscriber.fileno()
        if fd in self._subscribers:
            try:
                self._epoll.unregister(fd)
            except OSError as exc:
                raise EventLoopError(
                    f"unable to unregister fd {fd}: {exc}") from exc
            self._subscribers.pop(fd, None)

    def run_once(self, timeout: float | None = None) -> int:
        """Poll once and deliver callbacks."""

        count = 0
        for fd, events in self._epoll.poll(timeout):
            subscriber = self._subscribers.get(fd)
            if subscriber is None:
                raise EventLoopError(f"unknown subscriber fd {fd}")
            subscriber.on_event(events)
            count += 1
        return count

    def close(self) -> None:
        """Release the epoll fd."""

        if getattr(self, "_epoll", None) is not None:
            self._epoll.close()
