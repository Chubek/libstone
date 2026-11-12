// GUIDE 14.2: the original C++ example on the C++ wrapper.
#include <memtkx/MemTKX.hpp>

#include <iostream>
#include <new>
#include <vector>

struct Node {
  MemTKX::Address next{0};
  int value{0};
};

int main() {
  using namespace MemTKX::literals;
  MemTKX::MarkSweepPlan heap(64_KB);

  auto allocation = heap.allocate(sizeof(Node), alignof(Node));
  if (allocation.is_err()) {
    return 1;
  }
  const auto root = allocation.unwrap();
  auto* node = new (MemTKX::from_address<void>(root)) Node{};
  node->value = 42;

  MemTKX::TraceFn trace = [](MemTKX::Address address) {
    const auto* current = MemTKX::from_address<const Node>(address);
    std::vector<MemTKX::Address> children;
    if (current->next != 0) {
      children.push_back(current->next);
    }
    return children;
  };

  auto collected = heap.collect({root}, trace);
  if (collected.is_err()) {
    return 2;
  }
  const auto stats = collected.unwrap();
  std::cout << "value=" << node->value << ", live_objects=" << stats.live_objects
            << ", collections=" << stats.collection_count << '\n';
  node->~Node();
  return heap.collect({}, trace).is_ok() ? 0 : 3;
}
