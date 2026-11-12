#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace moonstone::plaid::conduit {
struct WakeSignal {
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t generation = 0;
    void notify() {
        {
            std::lock_guard lock(mutex);
            ++generation;
        }
        cv.notify_all();
    }
};
// Capacity 0 means synchronous rendezvous; capacity > 0 is a bounded FIFO.
// Close rejects new sends and allows already committed messages to drain.
template <class T> class Channel {
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>,
                  "channel messages require noexcept move construction and destruction");
    struct Offer {
        T value;
        bool received = false;
        explicit Offer(T &&item) : value(std::move(item)) {}
    };
    mutable std::mutex mutex_;
    std::condition_variable can_send_, can_receive_;
    std::deque<T> queue_;
    std::shared_ptr<Offer> rendezvous_;
    using Observers = std::list<std::weak_ptr<WakeSignal>>;
    Observers observers_;
    std::size_t capacity_;
    std::size_t waiting_receivers_ = 0;
    bool closed_ = false;
    void notify_observers_locked() {
        // No allocation after committing a message or close transition.
        for (auto it = observers_.begin(); it != observers_.end();) {
            if (auto shared = it->lock()) {
                shared->notify();
                ++it;
            } else
                it = observers_.erase(it);
        }
    }
    typename Observers::iterator subscribe(const std::shared_ptr<WakeSignal> &signal) {
        std::lock_guard lock(mutex_);
        if (!signal)
            throw std::invalid_argument("null channel observer");
        observers_.remove_if([](const auto &observer) { return observer.expired(); });
        return observers_.insert(observers_.end(), signal);
    }
    void unwatch(typename Observers::iterator observer) {
        std::lock_guard lock(mutex_);
        observers_.erase(observer);
    }
    std::optional<T> take_locked() {
        if (capacity_ == 0) {
            if (!rendezvous_)
                return std::nullopt;
            std::optional<T> item(std::in_place, std::move(rendezvous_->value));
            rendezvous_->received = true;
            rendezvous_.reset();
            can_send_.notify_all();
            notify_observers_locked();
            return item;
        }
        if (queue_.empty())
            return std::nullopt;
        std::optional<T> item(std::in_place, std::move(queue_.front()));
        queue_.pop_front();
        can_send_.notify_one();
        notify_observers_locked();
        return item;
    }

  public:
    struct ReceiveState {
        std::optional<T> value;
        bool closed;
    };
    // The channel must outlive this registration. Move-only, nonblocking
    // apart from mutex acquisition; destruction unsubscribes on all exits.
    class WatchRegistration {
        Channel *channel_;
        std::shared_ptr<WakeSignal> signal_;
        typename Observers::iterator observer_;
        friend class Channel;
        WatchRegistration(Channel &channel, std::shared_ptr<WakeSignal> signal)
            : channel_(&channel), signal_(std::move(signal)),
              observer_(channel_->subscribe(signal_)) {}

      public:
        WatchRegistration(const WatchRegistration &) = delete;
        WatchRegistration &operator=(const WatchRegistration &) = delete;
        WatchRegistration(WatchRegistration &&other) noexcept
            : channel_(std::exchange(other.channel_, nullptr)), signal_(std::move(other.signal_)),
              observer_(other.observer_) {}
        WatchRegistration &operator=(WatchRegistration &&) = delete;
        ~WatchRegistration() {
            if (channel_)
                channel_->unwatch(observer_);
        }
    };
    explicit Channel(std::size_t capacity) : capacity_(capacity) {}
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    std::size_t capacity() const noexcept { return capacity_; }
    bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }
    void watch(const std::shared_ptr<WakeSignal> &signal) { (void)subscribe(signal); }
    [[nodiscard]] WatchRegistration observe(std::shared_ptr<WakeSignal> signal) {
        return WatchRegistration(*this, std::move(signal));
    }
    std::size_t observer_count() const {
        std::lock_guard lock(mutex_);
        return observers_.size();
    }
    bool try_send(T value) {
        std::unique_lock lock(mutex_);
        if (closed_)
            return false;
        if (capacity_ == 0) {
            if (waiting_receivers_ == 0 || rendezvous_)
                return false;
            rendezvous_ = std::make_shared<Offer>(std::move(value));
        } else {
            if (queue_.size() >= capacity_)
                return false;
            queue_.push_back(std::move(value));
        }
        notify_observers_locked();
        lock.unlock();
        can_receive_.notify_one();
        return true;
    }
    bool send(T value) {
        std::unique_lock lock(mutex_);
        if (capacity_ == 0) {
            can_send_.wait(lock,
                           [&] { return closed_ || (waiting_receivers_ > 0 && !rendezvous_); });
            if (closed_)
                return false;
            auto offer = std::make_shared<Offer>(std::move(value));
            rendezvous_ = offer;
            notify_observers_locked();
            can_receive_.notify_one();
            // A successful synchronous send completes only when received.
            can_send_.wait(lock, [&] { return offer->received; });
            return true;
        }
        can_send_.wait(lock, [&] { return closed_ || queue_.size() < capacity_; });
        if (closed_)
            return false;
        queue_.push_back(std::move(value));
        notify_observers_locked();
        lock.unlock();
        can_receive_.notify_one();
        return true;
    }
    std::optional<T> try_receive() { return poll_receive().value; }
    // Value and closed-and-empty must be sampled under one lock by select.
    ReceiveState poll_receive() {
        std::unique_lock lock(mutex_);
        auto item = take_locked();
        const bool drained = !item && closed_;
        return {std::move(item), drained};
    }
    std::optional<T> receive() {
        std::unique_lock lock(mutex_);
        ++waiting_receivers_;
        can_send_.notify_one();
        can_receive_.wait(lock, [&] {
            return closed_ || (capacity_ == 0 ? bool(rendezvous_) : !queue_.empty());
        });
        --waiting_receivers_;
        return take_locked();
    }
    void close() {
        std::unique_lock lock(mutex_);
        if (closed_)
            return;
        closed_ = true;
        notify_observers_locked();
        lock.unlock();
        can_receive_.notify_all();
        can_send_.notify_all();
    }
};
} // namespace moonstone::plaid::conduit
