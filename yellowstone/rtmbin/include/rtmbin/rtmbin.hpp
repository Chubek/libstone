#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rtmbin {

enum class ErrorCode { Io, Unsupported, Malformed, Limit, InvalidInput, Overflow };

struct Error {
  ErrorCode code;
  std::string message;
};

template <class T>
using Result = std::variant<T, Error>;

enum class Format { ELF, PE, MachO };
std::string_view name(Format format) noexcept;

// Declared image geometry, not a prediction of instruction boundaries or
// live page permissions. Sizes are lengths; the end is exclusive.
struct Range {
  std::string name;
  std::uint64_t linked_address;
  std::uint64_t memory_size;
  std::uint64_t file_offset;
  std::uint64_t file_size;
  bool executable;
};

enum class AttributionStatus {
  Attributed,
  OutsideRanges,
  NonExecutable,
  NotFileBacked,
  AddressOutOfRange,
};

struct Attribution {
  AttributionStatus status;
  std::optional<std::uint64_t> linked_pc;   // absent on address arithmetic failure
  std::optional<std::size_t> range_index;   // index into Image::ranges()
  std::optional<std::uint64_t> file_offset; // present only on attribution
};

struct Sample {
  std::uint64_t pc;
  std::uint64_t weight = 1;
};

struct PlanPoint {
  std::string name; // 1–64 ASCII letters, digits, '.', '_' or '-'
  std::uint64_t pc;
  std::uint64_t size = 1;
};

struct VerifiedPoint {
  std::string name;
  std::uint64_t linked_pc;
  std::uint64_t file_offset;
  std::uint64_t size;
  std::size_t range_index;
};

struct Profile {
  std::uint64_t total_weight = 0;
  std::uint64_t attributed_weight = 0;
  std::uint64_t outside_ranges_weight = 0;
  std::uint64_t non_executable_weight = 0;
  std::uint64_t not_file_backed_weight = 0;
  std::uint64_t address_out_of_range_weight = 0;
  std::vector<std::uint64_t> weight_by_range; // same order as Image::ranges()
};

inline constexpr std::uint64_t max_image_size = 256ull * 1024 * 1024;
inline constexpr std::size_t max_plan_points = 10000;

class Image {
 public:
  // Both factories retain only normalized metadata, not the input bytes.
  static Result<Image> from_bytes(std::span<const std::uint8_t> bytes);
  static Result<Image> from_file(const std::filesystem::path& path);

  Format format() const noexcept { return format_; }
  std::string_view architecture() const noexcept { return architecture_; }
  unsigned address_bits() const noexcept { return address_bits_; }
  std::uint64_t preferred_base() const noexcept { return preferred_base_; }
  // Sorted, nonoverlapping memory ranges. The returned span borrows *this.
  std::span<const Range> ranges() const noexcept { return ranges_; }

  // Without runtime_base, pc is a linked virtual address. Otherwise
  // runtime_base is the caller-supplied runtime address corresponding to
  // preferred_base(), not a slide or a file offset.
  Attribution attribute(std::uint64_t pc,
                        std::optional<std::uint64_t> runtime_base = {}) const noexcept;

  // Weights must be positive. On overflow or invalid input there is no partial
  // report. The classification buckets sum to total_weight.
  Result<Profile> profile(std::span<const Sample> samples,
                          std::optional<std::uint64_t> runtime_base = {}) const;

  // Validate each named span wholly within a single file-backed executable
  // range. Names must be unique; results preserve order. This checks geometry
  // only, not instructions, safety of a patch, or current file identity.
  Result<std::vector<VerifiedPoint>> verification_plan(
      std::span<const PlanPoint> points,
      std::optional<std::uint64_t> runtime_base = {}) const;

 private:
  Image() = default;
  Format format_ = Format::ELF;
  std::string architecture_;
  unsigned address_bits_ = 0;
  std::uint64_t preferred_base_ = 0;
  std::vector<Range> ranges_;
};

using BlockId = std::uint32_t;

struct Block {
  BlockId id;
  std::vector<BlockId> successors; // zero, one, or two distinct block IDs
};

struct GraphSummary {
  std::vector<BlockId> reachable;
  std::vector<BlockId> unreachable;
  // Reachable cyclic strongly connected components; a self-loop counts.
  // Block IDs and the component list are sorted.
  std::vector<std::vector<BlockId>> reachable_cycles;
};

inline constexpr std::size_t max_model_blocks = 1024;

// Analyze only the supplied structural graph. Block IDs are abstract labels;
// no edges, conditions, or instruction semantics are inferred from an image.
Result<GraphSummary> summarize_graph(BlockId entry, std::span<const Block> blocks);

} // namespace rtmbin
