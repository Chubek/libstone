#pragma once

#include "WASM-Core.hpp"

namespace limestone::wasmutils::core {

struct FunctionType {
  std::vector<Type> params, results;
  bool operator==(const FunctionType&) const = default;
};

struct Limits {
  std::uint32_t min{};
  std::optional<std::uint32_t> max;
  bool operator==(const Limits&) const = default;
};

struct TableType {
  Type element = Type::funcref;
  Limits limits;
  bool operator==(const TableType&) const = default;
};

struct MemoryType {
  Limits limits;
  bool operator==(const MemoryType&) const = default;
};

struct GlobalType {
  Type type{};
  bool mutable_{};
  bool operator==(const GlobalType&) const = default;
};

enum class ExternalKind : std::uint8_t { function, table, memory, global };

// The uint32_t alternative holds the function's type index. Alternatives
// are ordered by ExternalKind.
using ImportDesc = std::variant<std::uint32_t, TableType, MemoryType, GlobalType>;

struct Import {
  std::string module, name;
  ImportDesc desc;
};

struct Function {
  std::uint32_t type{};
  FunctionBody body;
};

struct Global {
  GlobalType type;
  Program init;
};

struct Export {
  std::string name;
  ExternalKind kind{};
  std::uint32_t index{};
};

enum class SegmentMode { active, passive, declarative };

struct Element {
  SegmentMode mode{};
  std::uint32_t table{}; // Used only for active segments.
  Program offset;       // Used only for active segments.
  Type type = Type::funcref;
  std::vector<Program> init; // Legacy function indices become ref.func expressions.
};

struct Data {
  SegmentMode mode{};
  std::uint32_t memory{}; // Used only for active segments.
  Program offset;        // Used only for active segments.
  std::vector<std::uint8_t> bytes;
};

struct CustomSection {
  std::string name;
  std::vector<std::uint8_t> bytes;
};

struct Module {
  std::vector<FunctionType> types;
  std::vector<Import> imports;
  std::vector<Function> functions;
  std::vector<TableType> tables;
  std::vector<MemoryType> memories;
  std::vector<Global> globals;
  std::vector<Export> exports;
  std::optional<std::uint32_t> start;
  std::vector<Element> elements;
  std::vector<Data> data;
  std::optional<std::uint32_t> data_count;
  std::vector<CustomSection> custom;
};

// Parse the Core 2.0 binary grammar, with module-relative source offsets.
// Types, index ranges and constant-expression restrictions are not validated.
// Post-Core-2.0 binary forms are rejected.
Module decode_module(std::span<const std::uint8_t> bytes);

// Write canonical LEB128, omitting empty vector sections and using compact
// segment forms where applicable. Custom section names, payloads and relative
// order are preserved; custom sections are emitted last.
std::vector<std::uint8_t> encode_module(const Module& module);

} // namespace limestone::wasmutils::core
