#pragma once
/**
 * @file memtkx/barrier/satb.hpp
 * @brief `SatbQueue` and `SatbBarrier` (wrap memtkx/barrier/satb.h).
 */

#include <cstddef>
#include <vector>

#include "memtkx/barrier/satb.h"
#include "memtkx/core/types.hpp"

namespace MemTKX {

/** Queue of overwritten references; non-copyable. */
class SatbQueue {
 public:
  SatbQueue() noexcept { mtkx_satb_queue_init(&q_); }
  SatbQueue(const SatbQueue&) = delete;
  SatbQueue& operator=(const SatbQueue&) = delete;
  ~SatbQueue() { mtkx_satb_queue_destroy(&q_); }

  void push(Address object) { detail::throw_if_nomem(mtkx_satb_queue_push(&q_, object)); }

  /** Moves the entries out and empties the queue. */
  [[nodiscard]] std::vector<Address> take_entries() {
    std::vector<Address> out(q_.entries.data, q_.entries.data + q_.entries.size);
    mtkx_satb_queue_clear(&q_);
    return out;
  }

  [[nodiscard]] bool empty() const noexcept { return mtkx_satb_queue_empty(&q_); }
  [[nodiscard]] std::size_t size() const noexcept { return mtkx_satb_queue_size(&q_); }
  void clear() noexcept { mtkx_satb_queue_clear(&q_); }

  [[nodiscard]] mtkx_satb_queue* c_queue() noexcept { return &q_; }

 private:
  mtkx_satb_queue q_;
};

/** Captures the old reference before a store. */
class SatbBarrier {
 public:
  explicit SatbBarrier(SatbQueue& queue) noexcept : b_(mtkx_satb_barrier_make(queue.c_queue())) {}

  Address capture(Address* slot_address) { return mtkx_satb_barrier_capture(&b_, slot_address); }
  void write(Address* slot_address, Address new_value) {
    mtkx_satb_barrier_write(&b_, slot_address, new_value);
  }

 private:
  mtkx_satb_barrier b_;
};

}  // namespace MemTKX
