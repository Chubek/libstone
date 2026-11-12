// GUIDE 14.3: a C++ plugin space driven through the C API.
#include <memtkx/MemTKX.hpp>

#include <cassert>
#include <cstring>

class CountingSpace final : public MemTKX::SpaceBase<CountingSpace> {
 public:
  CountingSpace(MemTKX::Address start, MemTKX::Address end)
      : SpaceBase(MemTKX::SpaceDescriptor{"counting", MemTKX::SpaceKind::BumpPointer,
                                          {start, end}, alignof(std::max_align_t)}),
        bump_(start, end) {}

  MemTKX::AllocResult<MemTKX::Address> allocate_impl(std::size_t bytes, std::size_t alignment) {
    auto r = bump_.allocate(bytes, alignment);
    if (r.is_ok()) {
      record_allocation(bytes);
    }
    return r;
  }

 private:
  MemTKX::BumpPointerAllocator bump_;
};

int main() {
  alignas(std::max_align_t) static unsigned char storage[1024];
  CountingSpace space(MemTKX::as_address(storage), MemTKX::as_address(storage) + sizeof storage);
  mtkx_space* c = space.c_space();                 // hand this to C code
  const mtkx_alloc_result r = mtkx_space_allocate(c, 64, 16);  // calls allocate_impl
  assert(r.ok);
  assert(std::strcmp(mtkx_space_name(c), "counting") == 0);
  assert(space.metrics().total_allocations == 1);  // one shared set of metrics
}
