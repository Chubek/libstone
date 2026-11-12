#pragma once
/**
 * @file memtkx/scheduler/work_packet.hpp
 * @brief `WorkPacket` and `WorkQueue` (wrap memtkx/scheduler/work_packet.h).
 */

#include <cstddef>
#include <cstdint>
#include <utility>

#include "memtkx/core/types.hpp"
#include "memtkx/scheduler/work_packet.h"

namespace MemTKX {

enum class WorkKind : std::uint8_t {
  RootScan = MTKX_WORK_ROOT_SCAN,
  ProcessEdge = MTKX_WORK_PROCESS_EDGE,
  Mark = MTKX_WORK_MARK,
  Sweep = MTKX_WORK_SWEEP,
  Release = MTKX_WORK_RELEASE,
  User = MTKX_WORK_USER,
};

struct WorkPacket {
  WorkKind kind{WorkKind::User};
  Address object{0};
  std::size_t extra{0};

  [[nodiscard]] mtkx_work_packet to_c() const noexcept {
    return mtkx_work_packet_make(static_cast<mtkx_work_kind>(kind), object, extra);
  }
  [[nodiscard]] static WorkPacket from_c(const mtkx_work_packet& p) noexcept {
    return {static_cast<WorkKind>(p.kind), p.object, p.extra};
  }
};

/** FIFO of packets (non-copyable). */
class WorkQueue {
 public:
  WorkQueue() noexcept { mtkx_work_queue_init(&q_); }
  WorkQueue(const WorkQueue&) = delete;
  WorkQueue& operator=(const WorkQueue&) = delete;
  ~WorkQueue() { mtkx_work_queue_destroy(&q_); }

  void push(WorkPacket packet) { detail::throw_if_nomem(mtkx_work_queue_push(&q_, packet.to_c())); }

  [[nodiscard]] bool try_pop(WorkPacket& packet) {
    mtkx_work_packet p;
    if (!mtkx_work_queue_try_pop(&q_, &p)) {
      return false;
    }
    packet = WorkPacket::from_c(p);
    return true;
  }

  [[nodiscard]] bool empty() const noexcept { return mtkx_work_queue_empty(&q_); }
  [[nodiscard]] std::size_t size() const noexcept { return mtkx_work_queue_size(&q_); }
  void clear() noexcept { mtkx_work_queue_clear(&q_); }

  /** Handles packets until empty, including ones pushed by @p fn. */
  template <typename Fn>
  std::size_t drain(Fn&& fn) {
    std::size_t processed = 0;
    WorkPacket packet;
    while (try_pop(packet)) {
      fn(packet);
      ++processed;
    }
    return processed;
  }

  [[nodiscard]] mtkx_work_queue* c_queue() noexcept { return &q_; }

 private:
  mtkx_work_queue q_;
};

}  // namespace MemTKX
