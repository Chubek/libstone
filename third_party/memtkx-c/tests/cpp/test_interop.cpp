// C/C++ interoperability of the wrapper: C++ plugins used through the C API,
// C objects used through C++ wrappers, and exception transport across C.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <memtkx/MemTKX.hpp>
#include <memtkx/memtkx.h>

namespace application {

// Guide 13.2: a quota space plugin, written exactly as for the original.
class QuotaSpace final : public MemTKX::SpaceBase<QuotaSpace> {
 public:
  QuotaSpace(std::string name, MemTKX::Address start, MemTKX::Address end, std::size_t quota)
      : MemTKX::SpaceBase<QuotaSpace>(MemTKX::SpaceDescriptor{
            std::move(name), MemTKX::SpaceKind::BumpPointer, {start, end},
            alignof(std::max_align_t)}),
        allocator_(start, end),
        quota_(quota) {}

  MemTKX::AllocResult<MemTKX::Address> allocate_impl(std::size_t bytes, std::size_t alignment) {
    if (bytes == 13) {
      throw std::runtime_error("unlucky size");
    }
    if (bytes > quota_ - charged_) {
      return MemTKX::AllocResult<MemTKX::Address>::from_err(MemTKX::AllocError::OutOfMemory);
    }
    auto result = allocator_.allocate(bytes, alignment);
    if (result.is_ok()) {
      charged_ += bytes;
      record_allocation(bytes);
    }
    return result;
  }

  std::size_t charged_bytes() const noexcept { return charged_; }

  void reset() noexcept {
    allocator_.reset();
    charged_ = 0;
    metrics_.reset();
  }

 private:
  MemTKX::BumpPointerAllocator allocator_;
  std::size_t quota_;
  std::size_t charged_{0};
};

// Guide 13.3: an observed plan plugin.
class ObservedPlan final : public MemTKX::PlanBase<ObservedPlan> {
 public:
  explicit ObservedPlan(std::size_t bytes) : inner_(bytes) {}
  constexpr MemTKX::PlanKind kind_value() const noexcept { return MemTKX::PlanKind::MarkSweep; }
  constexpr std::string_view name_value() const noexcept { return "ObservedMarkSweep"; }
  MemTKX::AllocResult<MemTKX::Address> allocate(std::size_t bytes, std::size_t alignment) {
    return inner_.allocate(bytes, alignment);
  }
  MemTKX::GcResult<MemTKX::PlanStats> collect_impl(const std::vector<MemTKX::Address>& roots,
                                                   const MemTKX::TraceFn& trace) {
    auto result = inner_.collect(roots, trace);
    if (result.is_ok()) {
      ++observations;
    }
    return result;
  }
  std::size_t observations{0};

 private:
  MemTKX::MarkSweepPlan inner_;
};

struct Node {
  MemTKX::Address next{0};
};

bool c_trace_node(mtkx_address object, mtkx_address_vec* children, void*) {
  const auto* n = mtkx_from_address(const Node, object);
  return n->next == 0 || mtkx_address_vec_push(children, n->next) == MTKX_OK;
}

bool c_failing_trace(mtkx_address, mtkx_address_vec*, void*) { return false; }

}  // namespace application

int main() {
  alignas(std::max_align_t) static std::array<MemTKX::Byte, 4096> storage{};
  const auto start = MemTKX::as_address(storage.data());

  // --- a C++ plugin space driven through the C API ---
  {
    application::QuotaSpace space("quota", start, start + storage.size(), 128);
    mtkx_space* c = space.c_space();
    assert(std::strcmp(mtkx_space_name(c), "quota") == 0);
    mtkx_alloc_result r = mtkx_space_allocate(c, 96, 16);  // C -> C++ allocate_impl
    assert(r.ok && space.charged_bytes() == 96);
    assert(mtkx_space_metrics_of(c)->total_allocations == 1);
    assert(space.metrics().live_bytes == 96);
    assert(!mtkx_space_allocate(c, 64, 16).ok);
    assert(mtkx_space_allocate(c, 8, 3).error == MTKX_ALLOC_MISALIGNED);
    assert(space.allocate(1, 3).is_err());
    bool threw = false;
    try {
      (void)space.allocate(13, 8);  // exception crosses C and is rethrown here
    } catch (const std::runtime_error& e) {
      threw = std::string(e.what()) == "unlucky size";
    }
    assert(threw);
    space.reset();
    assert(space.charged_bytes() == 0 && space.metrics().live_bytes == 0);
    assert(mtkx_space_metrics_of(c)->total_allocations == 0);
  }

  // --- built-in spaces expose their C view ---
  {
    MemTKX::FreeListSpace heap("objects", start, start + storage.size());
    auto a = heap.allocate(128, 16);
    assert(a.is_ok());
    assert(mtkx_free_list_space_deallocate(reinterpret_cast<mtkx_free_list_space*>(heap.c_space()),
                                           a.unwrap(), 128));
    assert(heap.metrics().live_bytes == 0);
    assert(heap.allocator().free_cells() == 1);
    assert(MemTKX::to_string(heap.kind()) == "free-list");
  }

  // --- a C++ plugin plan collected through the C API with a C trace ---
  {
    application::ObservedPlan heap(4096);
    mtkx_plan* c = heap.c_plan();
    assert(mtkx_plan_get_kind(c) == MTKX_PLAN_MARK_SWEEP);
    assert(std::strcmp(mtkx_plan_name(c), "ObservedMarkSweep") == 0);
    mtkx_alloc_result root = mtkx_plan_allocate(c, sizeof(application::Node), 16);
    mtkx_alloc_result child = mtkx_plan_allocate(c, sizeof(application::Node), 16);
    assert(root.ok && child.ok);
    mtkx_from_address(application::Node, root.value)->next = child.value;
    mtkx_from_address(application::Node, child.value)->next = 0;
    mtkx_plan_stats_result r =
        mtkx_plan_collect(c, &root.value, 1, mtkx_trace_make(application::c_trace_node, nullptr));
    assert(r.ok && r.value.live_objects == 2 && heap.observations == 1);
    r = mtkx_plan_collect(c, &root.value, 1,
                          mtkx_trace_make(application::c_failing_trace, nullptr));
    assert(!r.ok && r.error == MTKX_GC_INVALID_TRACE);
  }

  // --- built-in plans: C view and C++ trace exceptions ---
  {
    MemTKX::MarkSweepPlan heap(4096);
    auto a = heap.allocate(32, 8);
    assert(a.is_ok());
    mtkx_plan* c = heap.c_plan();
    assert(mtkx_plan_get_kind(c) == MTKX_PLAN_MARK_SWEEP);
    bool threw = false;
    try {
      (void)heap.collect({a.unwrap()},
                         [](MemTKX::Address) -> std::vector<MemTKX::Address> {
                           throw std::logic_error("bad layout");
                         });
    } catch (const std::logic_error&) {
      threw = true;
    }
    assert(threw);
    assert(heap.live_objects() == 1);  // the failed collection did not sweep
    auto ok = heap.collect({}, {});
    assert(ok.is_ok() && ok.unwrap().freed_objects == 1);
    MemTKX::MarkSweepPlan moved(std::move(heap));
    assert(moved.live_objects() == 0);
  }

  // --- C slot buffers through the C++ wrapper ---
  {
    MemTKX::Address field = 0;
    MemTKX::SlotBuffer buffer;
    MemTKX::AddressRange source{MemTKX::as_address(&field), MemTKX::as_address(&field) + 8};
    assert(MemTKX::run_write_barrier(MemTKX::Slot{&field, 0x1800}, source, {0x1000, 0x2000},
                                     buffer));
    assert(buffer.size() == 1 && buffer[0].target_object == 0x1800);
    auto stage = MemTKX::barrier_non_null();
    assert(!(MemTKX::Slot{nullptr, 1} | stage).has_value());
  }

  // --- coordinator callbacks: exceptions surface from collect_roots ---
  {
    MemTKX::CollectorCoordinator coordinator(1);
    MemTKX::ThreadRecord record;
    record.id = 7;
    record.name = "thrower";
    record.root_provider = []() -> std::vector<MemTKX::Address> {
      throw std::runtime_error("roots unavailable");
    };
    assert(coordinator.register_mutator(record));
    assert(!coordinator.register_mutator(record));
    bool threw = false;
    try {
      (void)coordinator.collect_roots();
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw);
    assert(coordinator.unregister_mutator(7));
    MemTKX::ThreadRecord good;
    good.id = 8;
    good.stack_scan = [](MemTKX::ThreadId id) { return std::vector<MemTKX::Address>{id * 10}; };
    good.root_provider = [] { return std::vector<MemTKX::Address>{1, 2}; };
    assert(coordinator.register_mutator(good));
    auto roots = coordinator.collect_roots();
    assert((roots == std::vector<MemTKX::Address>{80, 1, 2}));
    assert(mtkx_coordinator_registered_mutators(coordinator.c_coordinator()) == 1);
  }

  // --- phase exceptions stop the pipeline and propagate ---
  {
    int ran = 0;
    auto pipeline =
        MemTKX::make_phase("boom", [](MemTKX::PhaseState&) -> MemTKX::GcResult<int> {
          throw std::runtime_error("phase failed");
        }, false) |
        MemTKX::make_phase("after", [&](MemTKX::PhaseState&) {
          ++ran;
          return MemTKX::GcResult<int>::from_ok(0);
        });
    MemTKX::PhaseState state;
    bool threw = false;
    try {
      (void)pipeline.run(state);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw && ran == 0);
  }

  // --- AST: C trees adopted by C++ ---
  {
    mtkx_ast_node* raw = mtkx_ast_node_of("pair", 2, mtkx_ast_leaf("a", "1"), mtkx_ast_leaf("b", "2"));
    auto tree = MemTKX::dsl::ASTNode::adopt(raw);
    assert(tree.dump() == "(pair (a 1) (b 2))");
    assert(tree.children().size() == 2 && tree.child(1).value() == "2");
    MemTKX::dsl::ASTNode copy = tree.child(0);  // a deep, owning copy
    assert(copy.owns() && copy.dump() == "(a 1)");
    tree.append_child(MemTKX::dsl::leaf("c", 3));
    assert(tree.size() == 3 && tree.child(2).dump() == "(c 3)");
    auto taken = tree.take_child(0);
    assert(taken == copy && tree.size() == 2);
    bool threw = false;
    try {
      (void)tree.child(9);
    } catch (const std::out_of_range&) {
      threw = true;
    }
    assert(threw);
  }

  // --- units and results ---
  {
    assert(MemTKX::MemoryUnitsDSL::parse_literal("1.5_MB") == 1.5L * 1024 * 1024);
    bool threw = false;
    try {
      (void)MemTKX::MemoryUnitsDSL::parse_literal("12");
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw);
    auto same = MemTKX::dsl::Result<int, int>::from_err(4);
    assert(same.is_err() && same.unwrap_err() == 4);
  }
  return 0;
}
