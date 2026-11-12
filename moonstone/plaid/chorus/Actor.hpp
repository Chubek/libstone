#pragma once
#include "../loom/Scheduler.hpp"
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace moonstone::plaid::chorus {
// Local actor: serialized mailbox processing on the supplied task executor.
// Its handler must not block waiting for a message from this same actor.
template <class Message> class Actor : public std::enable_shared_from_this<Actor<Message>> {
    static_assert(std::is_nothrow_move_constructible_v<Message> &&
                      std::is_nothrow_destructible_v<Message>,
                  "actor messages require noexcept move construction and destruction");
    loom::WorkStealingPool &executor_;
    std::function<void(Message)> handler_;
    mutable std::mutex mutex_;
    std::condition_variable idle_cv_;
    std::deque<Message> mailbox_;
    bool scheduled_ = false;
    bool accepting_ = true;
    std::exception_ptr failure_;
    const std::size_t mailbox_limit_;
    inline static thread_local Actor *current_actor_ = nullptr;
    void drain() {
        for (;;) {
            std::optional<Message> next;
            {
                std::unique_lock lock(mutex_);
                if (mailbox_.empty()) {
                    scheduled_ = false;
                    idle_cv_.notify_all();
                    return;
                }
                next.emplace(std::move(mailbox_.front()));
                mailbox_.pop_front();
            }
            auto *previous = current_actor_;
            current_actor_ = this;
            try {
                handler_(std::move(*next));
                current_actor_ = previous;
            } catch (...) {
                current_actor_ = previous;
                std::lock_guard lock(mutex_);
                failure_ = std::current_exception();
                accepting_ = false;
                mailbox_.clear();
                scheduled_ = false;
                idle_cv_.notify_all();
                return;
            }
        }
    }

  public:
    static std::shared_ptr<Actor> create(loom::WorkStealingPool &executor,
                                         std::function<void(Message)> handler,
                                         std::size_t mailbox_limit = 0) {
        if (!handler)
            throw std::invalid_argument("empty actor handler");
        return std::shared_ptr<Actor>(new Actor(executor, std::move(handler), mailbox_limit));
    }
    bool send(Message msg) {
        std::lock_guard lock(mutex_);
        if (!accepting_ || (mailbox_limit_ && mailbox_.size() >= mailbox_limit_))
            return false;
        mailbox_.push_back(std::move(msg));
        if (!scheduled_) {
            scheduled_ = true;
            auto self = this->shared_from_this();
            try {
                executor_.submit([self] { self->drain(); });
            } catch (...) {
                scheduled_ = false;
                mailbox_.pop_back();
                throw;
            }
        }
        return true;
    }
    void close() {
        std::lock_guard lock(mutex_);
        accepting_ = false;
    }
    void close_and_wait() {
        if (current_actor_ == this)
            throw std::logic_error("actor handler cannot wait for itself; use close");
        std::unique_lock lock(mutex_);
        accepting_ = false;
        idle_cv_.wait(lock, [&] { return !scheduled_; });
        if (failure_)
            std::rethrow_exception(failure_);
    }

  private:
    Actor(loom::WorkStealingPool &executor, std::function<void(Message)> handler,
          std::size_t mailbox_limit)
        : executor_(executor), handler_(std::move(handler)), mailbox_limit_(mailbox_limit) {}
};
} // namespace moonstone::plaid::chorus
