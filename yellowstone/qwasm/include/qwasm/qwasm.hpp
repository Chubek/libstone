#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace qwasm {

enum class ErrorKind { malformed, unsupported, limit_exceeded };

struct Error {
    ErrorKind kind;
    std::size_t offset;
    // Static diagnostic text; never contains bytes from the input.
    std::string_view message;
};

// Inclusive resource ceilings for one inspection.
struct InspectionLimits {
    std::size_t max_module_bytes = 64 * 1024 * 1024;
    std::size_t max_sections = 16 * 1024;
    std::size_t max_imports = 64 * 1024;
    std::size_t max_exports = 64 * 1024;
    std::size_t max_name_bytes = 64 * 1024;  // per name, in UTF-8 bytes
};

enum class SectionId : std::uint8_t {
    custom = 0, type, import, function, table, memory, global, export_,
    start, element, code, data, data_count, tag
};

struct ByteRange {
    std::size_t offset;
    std::size_t size;
};

struct CustomSection {
    std::string name;
    ByteRange data;  // Uninterpreted bytes after the name field.
};

struct Section {
    SectionId id;
    std::size_t offset;  // Section ID byte.
    ByteRange payload;   // Entire payload after the encoded section size.
    std::optional<CustomSection> custom;
};

enum class ExternalKind : std::uint8_t { function = 0, table, memory, global, tag };
enum class ReferenceType : std::uint8_t { funcref = 0x70, externref = 0x6f };
enum class ValueType : std::uint8_t {
    i32 = 0x7f, i64 = 0x7e, f32 = 0x7d, f64 = 0x7c, v128 = 0x7b,
    funcref = 0x70, externref = 0x6f
};

struct TypeLimits {
    std::uint64_t minimum;
    std::optional<std::uint64_t> maximum;
    bool address64;
};

struct FunctionImport { std::uint32_t type_index; };
struct TableImport { ReferenceType element_type; TypeLimits limits; };
struct MemoryImport { TypeLimits limits; bool shared; };
struct GlobalImport { ValueType value_type; bool is_mutable; };
struct TagImport { std::uint32_t type_index; };

using ImportDescriptor =
    std::variant<FunctionImport, TableImport, MemoryImport, GlobalImport, TagImport>;

struct Import {
    std::size_t offset;  // Beginning of the module name's length field.
    std::string module;
    std::string name;
    ImportDescriptor descriptor;
};

struct Export {
    std::size_t offset;  // Beginning of the name's length field.
    std::string name;
    ExternalKind kind;
    std::uint32_t index;  // As encoded; not resolved or checked.
};

struct Module {
    std::uint32_t version;
    std::vector<Section> sections;
    std::vector<Import> imports;
    std::vector<Export> exports;
    std::optional<std::uint32_t> start_function;
    std::optional<std::uint32_t> data_count;
};

using Inspection = std::variant<Module, Error>;

// Inspects core Wasm binary framing and selected metadata, not full module
// validity. All offsets are absolute and zero-based. The result owns its names;
// byte ranges refer to positions in the original input, not to borrowed storage.
// Input errors are returned as Error; allocation failure can throw.
[[nodiscard]] Inspection inspect(std::span<const std::uint8_t> bytes,
                                 const InspectionLimits& limits = {});

[[nodiscard]] std::string_view to_string(SectionId id) noexcept;
[[nodiscard]] std::string_view to_string(ExternalKind kind) noexcept;
[[nodiscard]] std::string_view to_string(ReferenceType type) noexcept;
[[nodiscard]] std::string_view to_string(ValueType type) noexcept;
[[nodiscard]] std::string_view to_string(ErrorKind kind) noexcept;

} // namespace qwasm
