#pragma once

#include "WASM-Instance.hpp"

namespace limestone::wasmutils::core::detail {

struct InstanceState;

struct MemoryInstance {
  explicit MemoryInstance(MemoryType type);
  [[nodiscard]] MemoryType type() const noexcept;
  std::uint32_t grow(std::uint32_t delta);

  std::vector<std::uint8_t> bytes;
  const std::optional<std::uint32_t> max_pages;
};

struct TableInstance {
  explicit TableInstance(TableType type);
  [[nodiscard]] TableType type() const noexcept;
  std::uint32_t grow(std::uint32_t delta, Value value);

  const Type element;
  const std::optional<std::uint32_t> max_entries;
  std::vector<Value> entries;
  // Non-owning: each state retains this table, and unregisters on destruction.
  // One registration per instance, regardless of aliasing in its index space.
  std::vector<InstanceState*> consumers;
};

struct GlobalInstance {
  const GlobalType type;
  Value value;
  const InstanceState* owner{}; // Only for an imported mutable funcref.
};

struct InstanceState {
  InstanceState() = default;
  ~InstanceState();
  InstanceState(const InstanceState&) = delete;
  InstanceState& operator=(const InstanceState&) = delete;

  [[nodiscard]] std::span<std::uint8_t> memory_bytes() const noexcept;
  void attach_table(const std::shared_ptr<TableInstance>& table, std::string_view name);
  void attach_global(const std::shared_ptr<GlobalInstance>& global, std::string_view name);

  std::shared_ptr<MemoryInstance> memory;
  std::vector<std::shared_ptr<TableInstance>> tables;
  std::vector<std::shared_ptr<GlobalInstance>> globals;
  std::vector<std::span<const std::uint8_t>> data;
  std::vector<std::vector<Value>> elements;
  std::uint64_t table_entries{};
};

// A function reference can only have an instance-local index. Otherwise this
// checks the host-facing raw Value representation, independent of the store.
bool match_value(Type type, const Value& value, std::size_t function_count = 0);

// The module is validated, its imports are in state, and it owns the bytes
// borrowed by passive data segments (so must outlive state).
void initialize_state(const Module& module, InstanceState& state);

// Execute a validated numeric, memory, table, reference, or global opcode.
// Instructions managed by the call/control interpreter return false untouched.
// An execution failure throws Trap at the instruction's source offset.
bool execute_opcode(const Instruction& instruction, std::vector<Value>& stack, InstanceState& state);

} // namespace limestone::wasmutils::core::detail
