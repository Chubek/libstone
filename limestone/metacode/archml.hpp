#pragma once
#include "metacode.hpp"
#include <map>

namespace limestone::metacode {

struct ArchMLOperand {
  std::string name;
  uint64_t value=0;
  bool is_signed=false;
};
struct ArchMLDecoded {
  std::string instruction;
  size_t size=0;
  std::vector<ArchMLOperand> operands;
};
struct ArchMLTiming {
  uint32_t latency=0;
  double throughput_cycles=0;
  bool barrier=false;
  std::vector<std::string> resources;
  SourceLocation source;
};
struct ArchMLSchedule {
  uint32_t issue_width=0;
  std::map<std::string,uint32_t> resource_capacity;
  std::map<std::string,ArchMLTiming> instructions;
};

// An immutable owning adapter. The Sandstone parser, AST and runtime
// descriptors are not part of the Limestone API.
class ArchMLProfile {
public:
  static Result<ArchMLProfile> parse(std::string_view text,std::string source="<memory>");
  static Result<ArchMLProfile> load(const std::string& path);
  // Borrowed views are valid for the lifetime of this profile or any copy.
  const std::string& name() const;
  uint32_t bits() const;
  std::string_view endianness() const;
  // Operands are in declaration order; signed values use uint64_t
  // two's-complement representation. Displacements stay byte-valued.
  Result<std::vector<uint8_t>> encode(std::string_view instruction,std::span<const uint64_t> operands) const;
  Result<ArchMLDecoded> decode_one(std::span<const uint8_t>) const;
  // Only instructions whose schedule class belongs to this microarchitecture
  // are included. No guessed timings or processor selection.
  Result<ArchMLSchedule> scheduling(std::string_view microarchitecture) const;
private:
  struct Data;
  explicit ArchMLProfile(std::shared_ptr<const Data> data):data_(std::move(data)){}
  std::shared_ptr<const Data> data_;
};

}
