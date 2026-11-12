#pragma once
/**
 * @file memtkx/detail/interop.hpp
 * @brief Internal plumbing of the C++ wrapper: exception transport across
 *        C frames and small ownership helpers. Not part of the public API.
 *
 * C++ callbacks invoked from C (trace functions, root providers, parser
 * functions, semantic actions ...) must never let an exception unwind
 * through C code. Every trampoline therefore catches, parks the exception
 * in a thread-local slot and reports failure to C; the wrapper rethrows
 * it once the C call has returned.
 */

#if __cplusplus < 201703L && !(defined(_MSVC_LANG) && _MSVC_LANG >= 201703L)
#error "The MemTKX C++ wrapper requires C++17 or later"
#endif

#include <exception>
#include <new>
#include <utility>

#include "memtkx/core/base.h"

namespace MemTKX::detail {

/** The exception parked by the most recent failing callback on this thread. */
inline std::exception_ptr& pending_exception() noexcept {
  thread_local std::exception_ptr slot;
  return slot;
}

/** @return true while a callback exception awaits rethrow. */
inline bool has_pending_exception() noexcept {
  return static_cast<bool>(pending_exception());
}

/** Parks the in-flight exception (call from inside a catch block). */
inline void park_current_exception() noexcept {
  if (!pending_exception()) {
    pending_exception() = std::current_exception();
  }
}

/** Rethrows (and clears) a parked exception, if any. */
inline void rethrow_pending() {
  if (pending_exception()) {
    std::exception_ptr e = pending_exception();
    pending_exception() = nullptr;
    std::rethrow_exception(e);
  }
}

/** Throws std::bad_alloc for a failed MemTKX status. */
inline void throw_if_nomem(mtkx_status status) {
  if (status == MTKX_ERR_NOMEM) {
    throw std::bad_alloc();
  }
}

/** Tag selecting a "wrap an existing C object" constructor. */
struct adopt_t {
  explicit adopt_t() = default;
};
inline constexpr adopt_t adopt{};

/**
 * @brief Value-initialised C struct held as a base class, so that it is
 *        constructed before the wrapper's other bases (base-from-member).
 */
template <typename C>
struct CStorage {
  C c_storage_{};
};

/** Move-only owner of a C object released by `Destroy`. */
template <typename T, void (*Destroy)(T*)>
class Handle {
 public:
  Handle() noexcept = default;
  explicit Handle(T* ptr) noexcept : ptr_(ptr) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.ptr_, nullptr));
    }
    return *this;
  }
  ~Handle() { reset(); }

  void reset(T* ptr = nullptr) noexcept {
    if (ptr_ != nullptr) {
      Destroy(ptr_);
    }
    ptr_ = ptr;
  }
  [[nodiscard]] T* get() const noexcept { return ptr_; }
  [[nodiscard]] T* release() noexcept { return std::exchange(ptr_, nullptr); }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

 private:
  T* ptr_{nullptr};
};

}  // namespace MemTKX::detail

namespace MemTKX::dsl::detail {
// The DSL headers use their own `detail` namespace; re-export the helpers.
using MemTKX::detail::has_pending_exception;
using MemTKX::detail::park_current_exception;
using MemTKX::detail::pending_exception;
using MemTKX::detail::rethrow_pending;
using MemTKX::detail::throw_if_nomem;
}  // namespace MemTKX::dsl::detail
