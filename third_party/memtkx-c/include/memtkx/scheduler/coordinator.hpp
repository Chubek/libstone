#pragma once
/**
 * @file memtkx/scheduler/coordinator.hpp
 * @brief `CollectorCoordinator`, `MutatorContext`, `CollectorContext`
 *        (wrap memtkx/scheduler/coordinator.h).
 *
 * Root and stack callbacks are std::function objects kept alive by the
 * wrapper while their record is registered. They run under the C
 * coordinator's mutex; exceptions they throw are rethrown from
 * `collect_roots()`.
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "memtkx/core/error.hpp"
#include "memtkx/core/types.hpp"
#include "memtkx/scheduler/coordinator.h"

namespace MemTKX {

using ThreadId = std::uint64_t;
using RootProvider = std::function<std::vector<Address>()>;
using StackScanCallback = std::function<std::vector<Address>(ThreadId)>;

enum class MutatorStatus : std::uint8_t {
  Running = MTKX_MUTATOR_RUNNING,
  Polling = MTKX_MUTATOR_POLLING,
  AtSafepoint = MTKX_MUTATOR_AT_SAFEPOINT,
  Blocked = MTKX_MUTATOR_BLOCKED,
};

struct ThreadRecord {
  ThreadId id{0};
  std::string name;
  MutatorStatus status{MutatorStatus::Running};
  StackScanCallback stack_scan;
  RootProvider root_provider;
};

namespace detail {
struct RecordCallbacks {
  StackScanCallback stack_scan;
  RootProvider root_provider;
};

inline mtkx_status append_roots(mtkx_address_vec* out, const std::vector<Address>& roots) {
  return mtkx_address_vec_append(out, roots.data(), roots.size());
}

inline mtkx_status stack_scan_trampoline(mtkx_thread_id id, mtkx_address_vec* out, void* ctx) {
  if (has_pending_exception()) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  try {
    return append_roots(out, static_cast<RecordCallbacks*>(ctx)->stack_scan(id));
  } catch (...) {
    park_current_exception();
    return MTKX_ERR_INVALID_ARGUMENT;
  }
}

inline mtkx_status root_provider_trampoline(mtkx_address_vec* out, void* ctx) {
  if (has_pending_exception()) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  try {
    return append_roots(out, static_cast<RecordCallbacks*>(ctx)->root_provider());
  } catch (...) {
    park_current_exception();
    return MTKX_ERR_INVALID_ARGUMENT;
  }
}
}  // namespace detail

/** Thread-safe mutator registry and safepoint coordinator. */
class CollectorCoordinator {
 public:
  explicit CollectorCoordinator(std::size_t expected_mutators = 1)
      : c_(mtkx_coordinator_create(expected_mutators)) {
    if (!c_) {
      throw std::bad_alloc();
    }
  }
  CollectorCoordinator(const CollectorCoordinator&) = delete;
  CollectorCoordinator& operator=(const CollectorCoordinator&) = delete;

  /** @return false when the id is already registered. */
  bool register_mutator(ThreadRecord record) {
    auto callbacks = std::make_unique<detail::RecordCallbacks>(
        detail::RecordCallbacks{std::move(record.stack_scan), std::move(record.root_provider)});
    mtkx_thread_record c_record;
    mtkx_thread_record_init(&c_record, record.id, record.name.c_str());
    c_record.status = static_cast<mtkx_mutator_status>(record.status);
    if (callbacks->stack_scan) {
      c_record.stack_scan = &detail::stack_scan_trampoline;
      c_record.stack_scan_ctx = callbacks.get();
    }
    if (callbacks->root_provider) {
      c_record.root_provider = &detail::root_provider_trampoline;
      c_record.root_provider_ctx = callbacks.get();
    }
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    const mtkx_status status = mtkx_coordinator_register_mutator(c_.get(), &c_record);
    if (status == MTKX_ERR_DUPLICATE) {
      return false;
    }
    detail::throw_if_nomem(status);
    callbacks_[record.id] = std::move(callbacks);
    return true;
  }

  bool unregister_mutator(ThreadId id) {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    const bool removed = mtkx_coordinator_unregister_mutator(c_.get(), id);
    if (removed) {
      callbacks_.erase(id);
    }
    return removed;
  }

  void update_status(ThreadId id, MutatorStatus status) {
    mtkx_coordinator_update_status(c_.get(), id, static_cast<mtkx_mutator_status>(status));
  }

  [[nodiscard]] MutatorStatus status(ThreadId id) const {
    return static_cast<MutatorStatus>(mtkx_coordinator_status(c_.get(), id));
  }

  [[nodiscard]] GcResult<int> request_stop_the_world(
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto ms = timeout.count() < 0 ? 0 : static_cast<std::uint64_t>(timeout.count());
    return detail::to_cpp(mtkx_coordinator_request_stop_the_world(c_.get(), ms));
  }

  void release_mutators() { mtkx_coordinator_release_mutators(c_.get()); }

  [[nodiscard]] GcResult<int> poll_safepoint(ThreadId id) {
    return detail::to_cpp(mtkx_coordinator_poll_safepoint(c_.get(), id));
  }

  [[nodiscard]] std::vector<Address> collect_roots() {
    mtkx_address_vec roots = MTKX_ADDRESS_VEC_INIT;
    const mtkx_status status = mtkx_coordinator_collect_roots(c_.get(), &roots);
    std::vector<Address> out(roots.data, roots.data + roots.size);
    mtkx_address_vec_destroy(&roots);
    detail::rethrow_pending();
    detail::throw_if_nomem(status);
    return out;
  }

  [[nodiscard]] std::size_t registered_mutators() const {
    return mtkx_coordinator_registered_mutators(c_.get());
  }

  [[nodiscard]] mtkx_coordinator* c_coordinator() const noexcept { return c_.get(); }

 private:
  detail::Handle<mtkx_coordinator, &mtkx_coordinator_destroy> c_;
  std::mutex callbacks_mutex_;
  std::map<ThreadId, std::unique_ptr<detail::RecordCallbacks>> callbacks_;
};

class MutatorContext {
 public:
  MutatorContext(CollectorCoordinator& coordinator, ThreadId id)
      : coordinator_(coordinator), id_(id) {}

  [[nodiscard]] GcResult<int> poll() { return coordinator_.poll_safepoint(id_); }
  [[nodiscard]] constexpr ThreadId id() const noexcept { return id_; }

 private:
  CollectorCoordinator& coordinator_;
  ThreadId id_;
};

class CollectorContext {
 public:
  explicit CollectorContext(CollectorCoordinator& coordinator) : coordinator_(coordinator) {}

  [[nodiscard]] GcResult<int> stop_the_world(
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    return coordinator_.request_stop_the_world(timeout);
  }
  void release_mutators() { coordinator_.release_mutators(); }
  [[nodiscard]] std::vector<Address> roots() { return coordinator_.collect_roots(); }

 private:
  CollectorCoordinator& coordinator_;
};

}  // namespace MemTKX
