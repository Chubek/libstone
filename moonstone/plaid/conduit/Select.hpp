#pragma once
#include "Channel.hpp"
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <vector>

namespace moonstone::plaid::conduit {
class SelectCancelled : public std::runtime_error {
  public:
    SelectCancelled() : std::runtime_error("receive select cancelled") {}
};
template <class T> struct ReceiveChoice {
    std::size_t index;
    std::optional<T> value;
    bool closed = false;
};
// Receive-only select with rotating fairness and event notification rather
// than polling. Closed-and-empty returns a choice with closed=true; only
// timeout returns nullopt. Cancellation throws SelectCancelled.
template <class T>
std::optional<ReceiveChoice<T>> select_receive(const std::vector<Channel<T> *> &channels,
                                               std::chrono::steady_clock::duration timeout,
                                               std::size_t rotation = 0,
                                               std::stop_token cancellation = {}) {
    if (channels.empty())
        throw std::invalid_argument("select requires cases");
    auto wake = std::make_shared<WakeSignal>();
    std::vector<typename Channel<T>::WatchRegistration> registrations;
    registrations.reserve(channels.size());
    for (auto *channel : channels) {
        if (!channel)
            throw std::invalid_argument("null select channel");
        if (channel->capacity() == 0)
            throw std::invalid_argument(
                "rendezvous select requires atomic registration; unsupported");
        registrations.push_back(channel->observe(wake));
    }
    std::stop_callback on_cancel(cancellation, [wake] { wake->notify(); });
    const auto now = std::chrono::steady_clock::now();
    const auto latest = std::chrono::steady_clock::time_point::max();
    const auto deadline = timeout <= std::chrono::steady_clock::duration::zero() ? now
                          : now >= latest - timeout                              ? latest
                                                                                 : now + timeout;
    std::size_t start = rotation % channels.size();
    while (true) {
        std::unique_lock epoch_lock(wake->mutex);
        auto generation = wake->generation;
        epoch_lock.unlock();
        if (cancellation.stop_requested())
            throw SelectCancelled();
        for (std::size_t offset = 0; offset < channels.size(); ++offset) {
            const auto i = (start + offset) % channels.size();
            auto state = channels[i]->poll_receive();
            if (state.value)
                return ReceiveChoice<T>{i, std::move(state.value), false};
            if (state.closed)
                return ReceiveChoice<T>{i, std::nullopt, true};
        }
        // A noisy channel must not extend the deadline indefinitely just
        // because the generation predicate is always true when we reach wait.
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::unique_lock lock(wake->mutex);
        if (!wake->cv.wait_until(lock, deadline, [&] { return wake->generation != generation; }))
            return std::nullopt;
        start = (start + 1) % channels.size();
    }
}
} // namespace moonstone::plaid::conduit
