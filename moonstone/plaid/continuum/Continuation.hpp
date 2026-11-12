#pragma once
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace moonstone::plaid::continuum {
enum class State : unsigned char { Ready, Running, Completed, Failed, Cancelled };
enum class ResumeResult : unsigned char { Resumed, AlreadyConsumed, Cancelled, Failed };

// A portable one-shot, *delimited callback continuation*. This does not capture
// arbitrary native stacks and is deliberately not Scheme call/cc.
template<class T> class OneShot {
    struct Control {
        explicit Control(std::function<void(T)> fn): operation(std::move(fn)) {
            if (!operation) throw std::invalid_argument("continuation requires a callback");
        }
        std::atomic<State> state{State::Ready};
        std::function<void(T)> operation;
        std::exception_ptr failure;
    };
    std::shared_ptr<Control> control_;
public:
    explicit OneShot(std::function<void(T)> fn): control_(std::make_shared<Control>(std::move(fn))) {}
    OneShot() = default;
    State state() const noexcept { return control_ ? control_->state.load(std::memory_order_acquire) : State::Cancelled; }
    bool valid() const noexcept { return static_cast<bool>(control_); }
    bool cancel() noexcept {
        if (!control_) return false;
        State expected = State::Ready;
        return control_->state.compare_exchange_strong(expected, State::Cancelled, std::memory_order_acq_rel);
    }
    ResumeResult resume(T value) noexcept {
        if (!control_) return ResumeResult::Cancelled;
        State expected = State::Ready;
        if (!control_->state.compare_exchange_strong(expected, State::Running, std::memory_order_acq_rel)) {
            return expected == State::Cancelled ? ResumeResult::Cancelled : ResumeResult::AlreadyConsumed;
        }
        try {
            control_->operation(std::move(value));
            control_->state.store(State::Completed, std::memory_order_release);
            return ResumeResult::Resumed;
        } catch (...) {
            control_->failure = std::current_exception();
            control_->state.store(State::Failed, std::memory_order_release);
            return ResumeResult::Failed;
        }
    }
    void rethrow_if_failed() const {
        if (state() == State::Failed) std::rethrow_exception(control_->failure);
    }
};

// Lifetime-checked prompt: continuations captured under a prompt may not
// resume after the enclosing prompt is invalidated. The actual capture point
// is supplied by a language compiler or a VM adapter.
class Prompt {
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
public:
    Prompt() = default;
    Prompt(const Prompt&) = delete;
    Prompt& operator=(const Prompt&) = delete;
    ~Prompt() { close(); }
    void close() noexcept { alive_->store(false, std::memory_order_release); }
    template<class T> OneShot<T> capture(std::function<void(T)> fn) const {
        std::weak_ptr<std::atomic<bool>> lifetime = alive_;
        return OneShot<T>([fn=std::move(fn), lifetime](T value) mutable {
            auto guard = lifetime.lock();
            if (!guard || !guard->load(std::memory_order_acquire))
                throw std::runtime_error("continuation prompt has ended");
            fn(std::move(value));
        });
    }
};
} // namespace moonstone::plaid::continuum
