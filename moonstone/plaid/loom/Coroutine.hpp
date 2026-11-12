#pragma once
#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace moonstone::plaid::loom {
// Explicitly driven stackless C++20 coroutine. The compiler synthesizes its
// frame. Ownership of the frame is exclusive to this object; no background
// scheduler may resume it concurrently or after destruction.
template<class T> class ManualCoroutine {
public:
    struct promise_type {
        std::optional<T> value;
        std::exception_ptr error;
        ManualCoroutine get_return_object() {
            return ManualCoroutine(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() const noexcept { return {}; }
        std::suspend_always final_suspend() const noexcept { return {}; }
        void return_value(T result) { value.emplace(std::move(result)); }
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };
private:
    std::coroutine_handle<promise_type> handle_;
    explicit ManualCoroutine(std::coroutine_handle<promise_type> h): handle_(h) {}
public:
    ManualCoroutine(ManualCoroutine&& other) noexcept: handle_(std::exchange(other.handle_,{})) {}
    ManualCoroutine& operator=(ManualCoroutine&& other) noexcept {
        if (this != &other) { if (handle_) handle_.destroy(); handle_=std::exchange(other.handle_,{}); }
        return *this;
    }
    ManualCoroutine(const ManualCoroutine&) = delete;
    ManualCoroutine& operator=(const ManualCoroutine&) = delete;
    ~ManualCoroutine() { if (handle_) handle_.destroy(); }
    bool done() const noexcept { return !handle_ || handle_.done(); }
    bool resume() {
        if (done()) return false;
        handle_.resume();
        if (handle_.promise().error) std::rethrow_exception(handle_.promise().error);
        return !handle_.done();
    }
    T result() const {
        if (!done() || !handle_) throw std::logic_error("coroutine not complete");
        if (handle_.promise().error) std::rethrow_exception(handle_.promise().error);
        return handle_.promise().value.value();
    }
};
} // namespace moonstone::plaid::loom
